#ifndef WUWE_AGENT_REASONING_RUNNER_HPP
#define WUWE_AGENT_REASONING_RUNNER_HPP

#include <algorithm>
#include <chrono>
#include <functional>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include <wuwe/agent/llm/llm_agent_runner.h>
#include <wuwe/agent/llm/llm_client.h>
#include <wuwe/agent/memory/memory_context.hpp>
#include <wuwe/agent/planning/planning.hpp>
#include <wuwe/agent/reasoning/reasoning_core.hpp>
#include <wuwe/agent/reflection/reflection.hpp>
#include <wuwe/agent/tools/tool.hpp>

namespace wuwe::agent::reasoning {

using reasoning_agent_complete =
  std::function<llm_response(llm_request, llm_agent_run_options, const reasoning_policy&)>;

struct reasoning_runner_options {
  llm_client* client {};
  reasoning_agent_complete agent_complete;
  std::shared_ptr<planning::planner> planner;
  std::shared_ptr<planning::plan_executor> executor;
  planning::plan_store* plan_store {};
  agent::memory::memory_context* memory {};
  std::shared_ptr<reflection::reflection_runner> reflection;
  reasoning_observer observer;
  std::function<bool()> should_cancel;
};

class reasoning_runner {
public:
  explicit reasoning_runner(reasoning_runner_options options) : options_(std::move(options)) {
    if (!options_.client && !options_.agent_complete &&
        (!options_.planner || !options_.executor)) {
      throw std::invalid_argument(
        "reasoning_runner requires client, agent_complete, or planner and executor");
    }
  }

  explicit reasoning_runner(llm_client& client, reasoning_observer observer = {})
      : reasoning_runner(reasoning_runner_options {
          .client = &client,
          .observer = std::move(observer),
        }) {
  }

  template<typename ToolProvider>
  static reasoning_runner with_tools(
    llm_client& client,
    std::shared_ptr<ToolProvider> provider,
    reasoning_runner_options options = {}) {
    options.client = &client;
    options.agent_complete =
      [&client, provider = std::move(provider)](
        llm_request request,
        llm_agent_run_options run_options,
        const reasoning_policy& policy) {
        if (policy.mode == reasoning_mode::simple) {
          llm_agent_runner runner(client, static_cast<int>(policy.budget.max_tool_rounds));
          return runner.complete(std::move(request), std::move(run_options));
        }
        llm_agent_runner runner(client, provider, static_cast<int>(policy.budget.max_tool_rounds));
        return runner.complete(std::move(request), std::move(run_options));
      };
    return reasoning_runner(std::move(options));
  }

  reasoning_result run(reasoning_request request) const {
    const auto started = std::chrono::steady_clock::now();
    auto state = std::make_shared<run_state>();
    state->started = started;
    state->budget = request.policy.budget;

    reasoning_result result;
    result.mode = request.policy.mode;
    emit({ .type = reasoning_event_type::started, .mode = result.mode }, state.get());

    try {
      switch (request.policy.mode) {
        case reasoning_mode::simple:
          result = run_model_once(request, request.input, state);
          break;
        case reasoning_mode::react:
          result = run_model_once(request, request.input, state);
          break;
        case reasoning_mode::reflect_and_retry:
          result = run_reflect_and_retry(request, state);
          break;
        case reasoning_mode::plan_execute:
          result = run_plan_execute(request, state);
          break;
      }
    }
    catch (const std::exception& ex) {
      result.mode = request.policy.mode;
      result.completed = false;
      result.error = ex.what();
      result.error_code = std::make_error_code(std::errc::operation_canceled);
    }

    result.elapsed = elapsed_since(started);
    if (state->budget_exceeded) {
      result.completed = false;
      result.error_code = std::make_error_code(std::errc::operation_canceled);
      result.error = state->budget_error;
    }
    result.usage = state->usage;
    emit({
      .type = result.completed ? reasoning_event_type::completed : reasoning_event_type::failed,
      .mode = result.mode,
      .message = result.completed ? result.content : result.error,
      .result = &result,
      .elapsed = result.elapsed,
    }, state.get());
    result.trace = std::move(state->trace);
    result.usage = state->usage;
    return result;
  }

private:
  struct run_state {
    std::chrono::steady_clock::time_point started;
    reasoning_budget budget;
    reasoning_usage usage;
    std::vector<reasoning_trace_record> trace;
    bool budget_exceeded { false };
    std::string budget_error;
  };

  reasoning_result run_model_once(
    const reasoning_request& request,
    std::string input,
    const std::shared_ptr<run_state>& state) const {
    reasoning_result result;
    result.mode = request.policy.mode;
    if (cancelled() || budget_cancelled(*state)) {
      return cancelled_result(result, state.get());
    }

    auto llm_request = make_llm_request(request, input);
    llm_agent_run_options run_options;
    run_options.callbacks =
      make_agent_callbacks(request.policy.mode, request.policy.enable_streaming, state);

    auto response = complete_agent(std::move(llm_request), std::move(run_options), request.policy);
    result.final_response = response;
    result.content = response.content;
    result.completed = static_cast<bool>(response);
    result.error_code = response.error_code;
    if (response.error_code) {
      result.error = response.error_code.message();
    }
    result.steps.push_back({
      .id = "model-1",
      .type = to_string(request.policy.mode),
      .input = std::move(input),
      .output = result.content,
      .error = result.error,
    });
    if (state->budget_exceeded) {
      result.completed = false;
      result.error_code = std::make_error_code(std::errc::operation_canceled);
      result.error = state->budget_error;
    }
    return result;
  }

  reasoning_result run_reflect_and_retry(
    reasoning_request request,
    const std::shared_ptr<run_state>& state) const {
    if (!options_.reflection) {
      throw std::invalid_argument("reflect_and_retry requires reflection runner");
    }

    auto policy = request.policy;
    std::string current_input = request.input;
    reasoning_result last;
    last.mode = reasoning_mode::reflect_and_retry;

    const auto max_attempts = (std::max<std::size_t>)(std::size_t { 1 },
      request.policy.budget.max_reflection_attempts);
    for (std::size_t attempt = 0; attempt < max_attempts; ++attempt) {
      if (budget_cancelled(*state)) {
        return cancelled_result(std::move(last), state.get());
      }
      request.policy = policy;
      auto candidate = run_model_once(request, current_input, state);
      last.final_response = candidate.final_response;
      last.content = candidate.content;
      last.error_code = candidate.error_code;
      last.error = candidate.error;
      last.steps.insert(last.steps.end(),
        std::make_move_iterator(candidate.steps.begin()),
        std::make_move_iterator(candidate.steps.end()));
      if (!candidate.completed) {
        return last;
      }

      if (!reserve_reflection_call(*state)) {
        last.completed = false;
        last.error = state->budget_error;
        last.error_code = std::make_error_code(std::errc::operation_canceled);
        return last;
      }
      emit({ .type = reasoning_event_type::reflection_started,
        .mode = reasoning_mode::reflect_and_retry,
        .message = candidate.content }, state.get());

      auto reflected = options_.reflection->run({
        .task = "Evaluate reasoning output",
        .original_input = request.input,
        .candidate_output = candidate.content,
        .subject_type = "reasoning_output",
        .rubric = request.rubric,
        .metadata = request.metadata,
      });
      last.reflections.push_back(reflected);
      emit({ .type = reasoning_event_type::reflection_completed,
        .mode = reasoning_mode::reflect_and_retry,
        .reflection_result = &last.reflections.back().result }, state.get());

      const auto action = reflected.result.recommended_action;
      if (action == reflection::reflection_action::pass) {
        last.completed = true;
        return last;
      }
      if (action == reflection::reflection_action::revise &&
          request.policy.return_revised_output && !reflected.result.revised_output.empty()) {
        last.content = reflected.result.revised_output;
        last.final_response.content = last.content;
        last.completed = true;
        return last;
      }
      if (action == reflection::reflection_action::block ||
          action == reflection::reflection_action::escalate ||
          action == reflection::reflection_action::replan) {
        last.completed = false;
        last.error = "reflection requested " + reflection::to_string(action);
        last.error_code = std::make_error_code(std::errc::operation_not_permitted);
        return last;
      }

      current_input = retry_prompt(request.input, candidate.content, reflected.result);
    }

    last.completed = false;
    last.error = "reflection retry budget exhausted";
    last.error_code = std::make_error_code(std::errc::resource_unavailable_try_again);
    return last;
  }

  reasoning_result run_plan_execute(
    const reasoning_request& request,
    const std::shared_ptr<run_state>& state) const {
    if (!options_.planner || !options_.executor) {
      throw std::invalid_argument("plan_execute requires planner and executor");
    }

    reasoning_result result;
    result.mode = reasoning_mode::plan_execute;

    auto policy = request.policy.planning;
    if (policy.max_steps == 0) {
      policy.max_steps = request.policy.budget.max_steps;
    }
    if (request.policy.allow_replanning) {
      policy.allow_replanning = true;
    }
    if (request.policy.budget.timeout.count() > 0 && policy.run_timeout.count() == 0) {
      policy.run_timeout = request.policy.budget.timeout;
    }

    planning::plan_runner runner({
      .planner = options_.planner,
      .executor = options_.executor,
      .store = options_.plan_store,
      .memory = options_.memory,
      .policy = policy,
      .should_cancel = [this, state] {
        return cancelled() || budget_cancelled(*state);
      },
      .observer = [this, mode = request.policy.mode, state](const planning::plan_event& event) {
        emit_plan_event(mode, event, state.get());
      },
    });

    auto plan_result = runner.run({
      .goal = request.input,
      .input = request.input,
      .system_prompt = request.system_prompt,
      .max_steps = request.policy.budget.max_steps,
      .metadata = request.metadata,
    });

    result.completed = plan_result.completed;
    result.content = plan_result.final_output;
    result.plan = plan_result;
    state->usage.plan_steps = (std::max)(state->usage.plan_steps, plan_result.steps_executed);
    if (!plan_result.completed) {
      result.error = plan_result.error.empty()
                       ? "plan stopped: " + planning::to_string(plan_result.stop_reason)
                       : plan_result.error;
      result.error_code = std::make_error_code(std::errc::operation_canceled);
    }
    return result;
  }

  llm_response complete_agent(
    llm_request request,
    llm_agent_run_options options,
    const reasoning_policy& policy) const {
    if (options_.agent_complete) {
      return options_.agent_complete(std::move(request), std::move(options), policy);
    }

    llm_agent_runner runner(*options_.client, static_cast<int>(policy.budget.max_tool_rounds));
    return runner.complete(std::move(request), std::move(options));
  }

  llm_request make_llm_request(const reasoning_request& request, const std::string& input) const {
    llm_request output;
    output.model = request.model;
    output.temperature = request.temperature;
    if (!request.system_prompt.empty()) {
      output.messages.push_back({ .role = "system", .content = request.system_prompt });
    }
    output.messages.push_back({ .role = "user", .content = input });
    return output;
  }

  llm_agent_callbacks make_agent_callbacks(
    reasoning_mode mode,
    bool enable_streaming,
    std::shared_ptr<run_state> state) const {
    llm_agent_callbacks callbacks;
    if (enable_streaming) {
      callbacks.on_delta = [this, mode, state](std::string_view delta) {
        emit({
          .type = reasoning_event_type::content_delta,
          .mode = mode,
          .delta = std::string(delta),
        }, state.get());
      };
    }
    callbacks.on_model_start = [this, mode, state](const llm_request& request) {
      const bool allowed = state && reserve_model_call(*state);
      emit({
        .type = reasoning_event_type::model_started,
        .mode = mode,
        .message = request.messages.empty() ? std::string {} : request.messages.back().content,
      }, state.get());
      return allowed;
    };
    callbacks.allow_tool_call = [this, state](const llm_tool_call&) {
      if (state) {
        return reserve_tool_call(*state);
      }
      return true;
    };
    callbacks.on_tool_start = [this, mode, state](const llm_tool_call& call) {
      emit({
        .type = reasoning_event_type::tool_started,
        .mode = mode,
        .message = call.name,
        .tool_call = &call,
      }, state.get());
    };
    callbacks.on_tool_result =
      [this, mode, state](const llm_tool_call& call, const llm_tool_result& result) {
        emit({
          .type = reasoning_event_type::tool_completed,
          .mode = mode,
          .message = result.content,
          .tool_call = &call,
          .tool_result = &result,
        }, state.get());
      };
    callbacks.on_cancelled = [this, mode, state] {
      emit({ .type = reasoning_event_type::cancelled, .mode = mode }, state.get());
    };
    return callbacks;
  }

  void emit_plan_event(reasoning_mode mode, const planning::plan_event& event, run_state* state) const {
    reasoning_event_type type = reasoning_event_type::plan_step_completed;
    switch (event.type) {
      case planning::plan_event_type::plan_created:
        type = reasoning_event_type::plan_created;
        break;
      case planning::plan_event_type::step_started:
        type = reasoning_event_type::plan_step_started;
        if (state) {
          ++state->usage.plan_steps;
        }
        break;
      case planning::plan_event_type::step_failed:
        type = reasoning_event_type::plan_step_failed;
        break;
      case planning::plan_event_type::step_blocked:
        type = reasoning_event_type::plan_step_blocked;
        break;
      case planning::plan_event_type::plan_revised:
        type = reasoning_event_type::plan_revised;
        break;
      case planning::plan_event_type::plan_cancelled:
        type = reasoning_event_type::cancelled;
        break;
      case planning::plan_event_type::plan_finished:
        return;
      default:
        type = reasoning_event_type::plan_step_completed;
        break;
    }

    emit({
      .type = type,
      .mode = mode,
      .step_id = event.step == nullptr ? std::string {} : event.step->id,
      .current_plan = event.current_plan,
      .plan_step = event.step,
    }, state);
  }

  reasoning_result cancelled_result(reasoning_result result, run_state* state) const {
    result.completed = false;
    result.error_code = std::make_error_code(std::errc::operation_canceled);
    result.error = state != nullptr && state->budget_exceeded
                     ? state->budget_error
                     : "reasoning cancelled";
    emit({ .type = reasoning_event_type::cancelled, .mode = result.mode, .message = result.error },
      state);
    return result;
  }

  bool cancelled() const {
    return options_.should_cancel && options_.should_cancel();
  }

  bool reserve_model_call(run_state& state) const {
    if (budget_cancelled(state)) {
      return false;
    }
    if (state.usage.model_calls >= state.budget.max_model_calls) {
      mark_budget_exceeded(state, "reasoning model call budget exceeded");
      return false;
    }
    ++state.usage.model_calls;
    return true;
  }

  bool reserve_reflection_call(run_state& state) const {
    if (budget_cancelled(state)) {
      return false;
    }
    if (state.usage.reflection_calls >= state.budget.max_reflection_attempts) {
      mark_budget_exceeded(state, "reasoning reflection budget exceeded");
      return false;
    }
    ++state.usage.reflection_calls;
    return true;
  }

  bool reserve_tool_call(run_state& state) const {
    if (budget_cancelled(state)) {
      return false;
    }
    if (state.usage.tool_calls >= state.budget.max_tool_calls) {
      mark_budget_exceeded(state, "reasoning tool call budget exceeded");
      return false;
    }
    ++state.usage.tool_calls;
    return true;
  }

  bool budget_cancelled(run_state& state) const {
    if (state.budget_exceeded) {
      return true;
    }
    if (state.budget.timeout.count() > 0 &&
        elapsed_since(state.started) >= state.budget.timeout) {
      mark_budget_exceeded(state, "reasoning timeout budget exceeded");
      return true;
    }
    return false;
  }

  void mark_budget_exceeded(run_state& state, std::string message) const {
    if (!state.budget_exceeded) {
      state.budget_exceeded = true;
      state.budget_error = std::move(message);
    }
  }

  void emit(reasoning_event event, run_state* state = nullptr) const {
    if (state) {
      if (event.elapsed.count() == 0) {
        event.elapsed = elapsed_since(state->started);
      }
      state->trace.push_back(trace_from_event(event, *state));
    }
    if (options_.observer) {
      options_.observer(event);
    }
  }

  static reasoning_trace_record trace_from_event(
    const reasoning_event& event,
    const run_state& state) {
    return {
      .sequence = state.trace.size() + 1,
      .type = event.type,
      .mode = event.mode,
      .step_id = event.step_id,
      .message = event.message,
      .delta = event.delta,
      .error = event.type == reasoning_event_type::failed ||
                   event.type == reasoning_event_type::cancelled
                 ? event.message
                 : std::string {},
      .elapsed = event.elapsed,
      .metadata = event.metadata,
    };
  }

  static std::string retry_prompt(
    const std::string& original_input,
    const std::string& previous_output,
    const reflection::reflection_result& reflection) {
    std::string feedback;
    for (const auto& issue : reflection.issues) {
      feedback += "- " + issue.code + ": " + issue.message;
      if (!issue.suggestion.empty()) {
        feedback += " Suggestion: " + issue.suggestion;
      }
      feedback += "\n";
    }

    return "Original request:\n" + original_input +
           "\n\nPrevious answer:\n" + previous_output +
           "\n\nReflection feedback:\n" + feedback +
           "\nPlease produce a corrected answer.";
  }

  static std::chrono::milliseconds elapsed_since(std::chrono::steady_clock::time_point started) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);
  }

  reasoning_runner_options options_;
};

} // namespace wuwe::agent::reasoning

#endif // WUWE_AGENT_REASONING_RUNNER_HPP
