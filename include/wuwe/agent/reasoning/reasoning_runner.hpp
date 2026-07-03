#ifndef WUWE_AGENT_REASONING_RUNNER_HPP
#define WUWE_AGENT_REASONING_RUNNER_HPP

#include <algorithm>
#include <chrono>
#include <exception>
#include <functional>
#include <future>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <wuwe/agent/knowledge/knowledge_tools.hpp>
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
  std::function<std::vector<llm_tool>()> available_tools;
  std::shared_ptr<reflection::reflection_runner> reflection;
  reasoning_observer observer;
  std::function<bool()> should_cancel;
};

class reasoning_run {
public:
  reasoning_run() = default;

  reasoning_run(std::jthread worker, std::future<reasoning_result> future)
      : worker_(std::move(worker)), future_(std::move(future)) {
  }

  reasoning_run(const reasoning_run&) = delete;
  reasoning_run& operator=(const reasoning_run&) = delete;
  reasoning_run(reasoning_run&&) noexcept = default;
  reasoning_run& operator=(reasoning_run&&) noexcept = default;

  bool valid() const noexcept {
    return future_.valid();
  }

  void request_stop() {
    worker_.request_stop();
  }

  bool stop_requested() const noexcept {
    return worker_.get_stop_token().stop_requested();
  }

  void wait() const {
    future_.wait();
  }

  reasoning_result get() {
    return future_.get();
  }

private:
  std::jthread worker_;
  std::future<reasoning_result> future_;
};

struct default_agentic_runner_options {
  std::string model;
  agent::memory::memory_context* memory {};
  std::shared_ptr<reflection::reflection_runner> reflection;
  std::shared_ptr<planning::planner> planner;
  std::shared_ptr<planning::plan_executor> executor;
  planning::plan_store* plan_store {};
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
    if (!options.available_tools) {
      options.available_tools = [provider] {
        return provider->tools();
      };
    }
    options.agent_complete =
      [&client, provider = std::move(provider), memory = options.memory](
        llm_request request,
        llm_agent_run_options run_options,
        const reasoning_policy& policy) {
        if (policy.mode == reasoning_mode::simple) {
          llm_agent_runner runner(
            client,
            memory,
            static_cast<int>(policy.budget.max_tool_rounds));
          return runner.complete(std::move(request), std::move(run_options));
        }
        llm_agent_runner runner(
          client,
          provider,
          memory,
          static_cast<int>(policy.budget.max_tool_rounds));
        return runner.complete(std::move(request), std::move(run_options));
      };
    return reasoning_runner(std::move(options));
  }

  reasoning_result run(reasoning_request request) const {
    return run(std::move(request), {});
  }

  reasoning_result run(reasoning_request request, reasoning_run_options run_options) const {
    const auto started = std::chrono::steady_clock::now();
    auto state = std::make_shared<run_state>();
    state->started = started;
    state->budget = request.policy.budget;
    state->usage.max_tool_rounds = request.policy.budget.max_tool_rounds;
    state->callbacks = std::move(run_options.callbacks);
    state->stop_token = run_options.stop_token;

    reasoning_result result;
    result.mode = request.policy.mode;
    notify({ .type = reasoning_event_type::started, .mode = result.mode }, state.get());

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
      result.reasoning_error =
        dynamic_cast<const std::invalid_argument*>(&ex) != nullptr
          ? reasoning_error_code::invalid_configuration
          : reasoning_error_code::unknown;
      result.error_code = make_error_code(result.reasoning_error);
    }

    result.elapsed = elapsed_since(started);
    if (state->budget_exceeded) {
      result.completed = false;
      result.reasoning_error = state->budget_error_code;
      result.error_code = make_error_code(result.reasoning_error);
      result.error = state->budget_error;
    }
    result.usage = state->usage;
    if (!result.completed && result.reasoning_error == reasoning_error_code::none) {
      result.reasoning_error = result.error_code == make_error_code(reasoning_error_code::cancelled)
                                 ? reasoning_error_code::cancelled
                                 : reasoning_error_code::unknown;
      if (!result.error_code) {
        result.error_code = make_error_code(result.reasoning_error);
      }
    }
    const auto terminal_type =
      result.completed ? reasoning_event_type::completed
                       : (result.reasoning_error == reasoning_error_code::cancelled
                            ? reasoning_event_type::cancelled
                            : reasoning_event_type::failed);
    state->defer_terminal_callbacks = true;
    notify({
      .type = terminal_type,
      .mode = result.mode,
      .message = result.completed ? result.content : result.error,
      .result = &result,
      .elapsed = result.elapsed,
      .metadata = result.final_response.metadata,
    }, state.get());
    result.trace = std::move(state->trace);
    result.usage = state->usage;
    state->defer_terminal_callbacks = false;
    dispatch_terminal_callbacks(result, state->callbacks);
    return result;
  }

  reasoning_run run_async(
    reasoning_request request,
    reasoning_run_options run_options = {}) const {
    auto promise = std::make_shared<std::promise<reasoning_result>>();
    auto future = promise->get_future();
    auto runner = *this;

    std::jthread worker(
      [runner = std::move(runner),
        request = std::move(request),
        run_options = std::move(run_options),
        promise](std::stop_token worker_stop_token) mutable {
        const auto external_stop_token = run_options.stop_token;
        std::stop_source run_stop_source;
        std::stop_callback external_stop_callback(
          external_stop_token,
          [&run_stop_source] {
            run_stop_source.request_stop();
          });
        std::stop_callback worker_stop_callback(
          worker_stop_token,
          [&run_stop_source] {
            run_stop_source.request_stop();
          });

        if (external_stop_token.stop_requested() || worker_stop_token.stop_requested()) {
          run_stop_source.request_stop();
        }

        run_options.stop_token = run_stop_source.get_token();
        try {
          promise->set_value(runner.run(std::move(request), std::move(run_options)));
        }
        catch (...) {
          promise->set_exception(std::current_exception());
        }
      });

    return reasoning_run(std::move(worker), std::move(future));
  }

private:
  struct run_state {
    std::chrono::steady_clock::time_point started;
    reasoning_budget budget;
    reasoning_callbacks callbacks;
    std::stop_token stop_token;
    reasoning_usage usage;
    std::vector<reasoning_trace_record> trace;
    bool budget_exceeded { false };
    bool defer_terminal_callbacks { false };
    bool emitted_reasoning_completed { false };
    reasoning_error_code budget_error_code { reasoning_error_code::unknown };
    std::string budget_error;
  };

  reasoning_result run_model_once(
    const reasoning_request& request,
    std::string input,
    const std::shared_ptr<run_state>& state) const {
    reasoning_result result;
    result.mode = request.policy.mode;
    if (cancelled(*state) || budget_cancelled(*state)) {
      return cancelled_result(result, state.get());
    }

    auto llm_request = make_llm_request(request, input);
    llm_agent_run_options run_options;
    run_options.callbacks =
      make_agent_callbacks(request.policy.mode, request.policy.enable_streaming, state);

    auto response = complete_agent(std::move(llm_request), std::move(run_options), request.policy);
    result.final_response = response;
    result.content = response.content;
    result.reasoning_summary = response.reasoning_summary;
    if (!response.reasoning_summary.empty() && !state->emitted_reasoning_completed) {
      notify({
        .type = reasoning_event_type::reasoning_completed,
        .mode = request.policy.mode,
        .reasoning_summary = response.reasoning_summary,
        .metadata = response.reasoning_metadata,
      }, state.get());
    }
    result.completed = static_cast<bool>(response);
    if (response.error_code) {
      result.underlying_error = response.error_code;
      result.reasoning_error = map_underlying_error(response.error_code);
      result.error_code = make_error_code(result.reasoning_error);
      result.error = response.content.empty() ? response.error_code.message() : response.content;
    }
    if (auto tool_rounds = metadata_size(response.metadata, "used_tool_rounds")) {
      state->usage.tool_rounds = (std::max)(state->usage.tool_rounds, *tool_rounds);
    }
    if (auto max_tool_rounds = metadata_size(response.metadata, "max_tool_rounds")) {
      state->usage.max_tool_rounds = (std::max)(state->usage.max_tool_rounds, *max_tool_rounds);
    }
    result.steps.push_back({
      .id = "model-1",
      .type = to_string(request.policy.mode),
      .input = std::move(input),
      .output = result.content,
      .error = result.error,
      .metadata = response.metadata,
    });
    if (state->budget_exceeded) {
      result.completed = false;
      result.reasoning_error = state->budget_error_code;
      result.error_code = make_error_code(result.reasoning_error);
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
      last.reasoning_summary = candidate.reasoning_summary;
      last.reasoning_error = candidate.reasoning_error;
      last.underlying_error = candidate.underlying_error;
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
        last.reasoning_error = state->budget_error_code;
        last.error_code = make_error_code(last.reasoning_error);
        return last;
      }
      notify({ .type = reasoning_event_type::reflection_started,
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
      notify({ .type = reasoning_event_type::reflection_completed,
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
        last.reasoning_error = reasoning_error_code::reflection_blocked;
        last.error_code = make_error_code(last.reasoning_error);
        return last;
      }

      current_input = retry_prompt(request.input, candidate.content, reflected.result);
    }

    last.completed = false;
    last.error = "reflection retry budget exhausted";
    last.reasoning_error = reasoning_error_code::reflection_budget_exceeded;
    last.error_code = make_error_code(last.reasoning_error);
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
      .stop_token = state->stop_token,
      .should_cancel = [this, state] {
        return cancelled(*state) || budget_cancelled(*state);
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
      .available_tools = options_.available_tools ? options_.available_tools() : std::vector<llm_tool> {},
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
      result.reasoning_error = reasoning_error_from_plan_stop(plan_result.stop_reason);
      result.error_code = make_error_code(result.reasoning_error);
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

    llm_agent_runner runner(
      *options_.client,
      options_.memory,
      static_cast<int>(policy.budget.max_tool_rounds));
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
        notify({
          .type = reasoning_event_type::content_delta,
          .mode = mode,
          .delta = std::string(delta),
        }, state.get());
      };
      callbacks.on_event = [this, mode, state](const llm_agent_event& event) {
        switch (event.type) {
          case llm_agent_event_type::model_first_event:
            notify({
              .type = reasoning_event_type::model_first_event,
              .mode = mode,
            }, state.get());
            break;
          case llm_agent_event_type::model_reasoning_delta:
            notify({
              .type = reasoning_event_type::reasoning_delta,
              .mode = mode,
              .reasoning_delta = event.delta,
              .metadata = event.stream_event ? event.stream_event->reasoning_metadata
                                             : std::map<std::string, std::string> {},
            }, state.get());
            break;
          case llm_agent_event_type::model_reasoning_completed:
            notify({
              .type = reasoning_event_type::reasoning_completed,
              .mode = mode,
              .reasoning_summary = event.message,
              .metadata = event.stream_event ? event.stream_event->reasoning_metadata
                                             : std::map<std::string, std::string> {},
            }, state.get());
            break;
          case llm_agent_event_type::tool_call_building:
            notify({
              .type = reasoning_event_type::tool_call_building,
              .mode = mode,
              .message = event.stream_event &&
                         event.stream_event->tool_call_delta.has_value()
                           ? event.stream_event->tool_call_delta->name_delta
                           : std::string {},
              .metadata = event.stream_event &&
                          event.stream_event->tool_call_delta.has_value()
                            ? std::map<std::string, std::string> {
                                { "tool_call_index",
                                  std::to_string(
                                    event.stream_event->tool_call_delta->index) },
                                { "arguments_delta",
                                  event.stream_event->tool_call_delta
                                    ->arguments_delta },
                              }
                            : std::map<std::string, std::string> {},
            }, state.get());
            break;
          case llm_agent_event_type::tool_call_ready:
            notify({
              .type = reasoning_event_type::tool_call_ready,
              .mode = mode,
              .message = event.tool_call ? event.tool_call->name : std::string {},
              .tool_call = event.tool_call,
            }, state.get());
            break;
          case llm_agent_event_type::model_completed:
            notify({
              .type = reasoning_event_type::model_completed,
              .mode = mode,
              .message = event.response ? event.response->finish_reason : std::string {},
              .metadata = event.response ? event.response->metadata
                                         : std::map<std::string, std::string> {},
            }, state.get());
            break;
          default:
            break;
        }
      };
    }
    callbacks.on_model_start = [this, mode, state](const llm_request& request) {
      const bool allowed = state && reserve_model_call(*state);
      notify({
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
      notify({
        .type = reasoning_event_type::tool_started,
        .mode = mode,
        .message = call.name,
        .tool_call = &call,
      }, state.get());
    };
    callbacks.on_tool_result =
      [this, mode, state](const llm_tool_call& call, const llm_tool_result& result) {
        notify({
          .type = reasoning_event_type::tool_completed,
          .mode = mode,
          .message = result.content,
          .tool_call = &call,
          .tool_result = &result,
        }, state.get());
      };
    callbacks.on_cancelled = [this, mode, state] {
      notify({ .type = reasoning_event_type::cancelled, .mode = mode }, state.get());
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

    notify({
      .type = type,
      .mode = mode,
      .step_id = event.step == nullptr ? std::string {} : event.step->id,
      .current_plan = event.current_plan,
      .plan_step = event.step,
    }, state);
  }

  reasoning_result cancelled_result(reasoning_result result, run_state* state) const {
    result.completed = false;
    auto event_type = reasoning_event_type::cancelled;
    if (state != nullptr && state->budget_exceeded) {
      result.reasoning_error = state->budget_error_code;
      result.error = state->budget_error;
      event_type = reasoning_event_type::failed;
    }
    else {
      result.reasoning_error = reasoning_error_code::cancelled;
      result.error = "reasoning cancelled";
    }
    result.error_code = make_error_code(result.reasoning_error);
    notify({ .type = event_type, .mode = result.mode, .message = result.error }, state);
    return result;
  }

  bool cancelled(const run_state& state) const {
    return state.stop_token.stop_requested() ||
           (options_.should_cancel && options_.should_cancel());
  }

  bool reserve_model_call(run_state& state) const {
    if (budget_cancelled(state)) {
      return false;
    }
    if (state.usage.model_calls >= state.budget.max_model_calls) {
      mark_budget_exceeded(
        state,
        reasoning_error_code::model_call_budget_exceeded,
        "reasoning model call budget exceeded");
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
      mark_budget_exceeded(
        state,
        reasoning_error_code::reflection_budget_exceeded,
        "reasoning reflection budget exceeded");
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
      mark_budget_exceeded(
        state,
        reasoning_error_code::tool_call_budget_exceeded,
        "reasoning tool call budget exceeded");
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
      mark_budget_exceeded(
        state,
        reasoning_error_code::timeout,
        "reasoning timeout budget exceeded");
      return true;
    }
    return false;
  }

  void mark_budget_exceeded(
    run_state& state,
    reasoning_error_code code,
    std::string message) const {
    if (!state.budget_exceeded) {
      state.budget_exceeded = true;
      state.budget_error_code = code;
      state.budget_error = std::move(message);
    }
  }

  void notify(reasoning_event event, run_state* state = nullptr) const {
    if (state) {
      if (event.type == reasoning_event_type::reasoning_completed) {
        state->emitted_reasoning_completed = true;
      }
      if (event.elapsed.count() == 0) {
        event.elapsed = elapsed_since(state->started);
      }
      state->trace.push_back(trace_from_event(event, *state));
    }
    if (options_.observer) {
      options_.observer(event);
    }
    if (!state) {
      return;
    }
    const auto& callbacks = state->callbacks;
    if (callbacks.on_event) {
      callbacks.on_event(event);
    }
    if (event.type == reasoning_event_type::content_delta && callbacks.on_delta &&
        !event.delta.empty()) {
      callbacks.on_delta(event.delta);
    }
    if (event.type == reasoning_event_type::reasoning_delta &&
        callbacks.on_reasoning_delta && !event.reasoning_delta.empty()) {
      callbacks.on_reasoning_delta(event.reasoning_delta);
    }
    if (event.type == reasoning_event_type::reasoning_completed &&
        callbacks.on_reasoning_done && !event.reasoning_summary.empty()) {
      callbacks.on_reasoning_done(event.reasoning_summary);
    }
    if (state->defer_terminal_callbacks &&
        (event.type == reasoning_event_type::completed ||
         event.type == reasoning_event_type::failed ||
         event.type == reasoning_event_type::cancelled)) {
      return;
    }
    if (event.type == reasoning_event_type::completed && callbacks.on_done && event.result) {
      callbacks.on_done(*event.result);
    }
    if (event.type == reasoning_event_type::failed && callbacks.on_error && event.result) {
      callbacks.on_error({
        .code = event.result->reasoning_error,
        .underlying_error = event.result->underlying_error,
        .message = event.result->error,
      });
    }
    if (event.type == reasoning_event_type::cancelled && callbacks.on_cancelled && event.result) {
      callbacks.on_cancelled(*event.result);
    }
  }

  static void dispatch_terminal_callbacks(
    const reasoning_result& result,
    const reasoning_callbacks& callbacks) {
    if (result.completed) {
      if (callbacks.on_done) {
        callbacks.on_done(result);
      }
      return;
    }

    if (result.reasoning_error == reasoning_error_code::cancelled) {
      if (callbacks.on_cancelled) {
        callbacks.on_cancelled(result);
      }
      return;
    }

    if (callbacks.on_error) {
      callbacks.on_error({
        .code = result.reasoning_error,
        .underlying_error = result.underlying_error,
        .message = result.error,
      });
    }
  }

  static reasoning_error_code map_underlying_error(std::error_code code) {
    if (!code) {
      return reasoning_error_code::none;
    }
    if (code == ::wuwe::agent::make_error_code(::wuwe::agent::llm_error_code::missing_api_key)) {
      return reasoning_error_code::missing_api_key;
    }
    if (code ==
        ::wuwe::agent::make_error_code(::wuwe::agent::llm_error_code::authentication_failed)) {
      return reasoning_error_code::authentication_failed;
    }
    if (code == ::wuwe::agent::make_error_code(::wuwe::agent::llm_error_code::rate_limited)) {
      return reasoning_error_code::rate_limited;
    }
    if (code == ::wuwe::agent::make_error_code(::wuwe::agent::llm_error_code::model_unavailable)) {
      return reasoning_error_code::model_unavailable;
    }
    if (code == ::wuwe::agent::make_error_code(::wuwe::agent::llm_error_code::cancelled) ||
        code == std::make_error_code(std::errc::operation_canceled)) {
      return reasoning_error_code::cancelled;
    }
    if (code == ::wuwe::agent::make_error_code(::wuwe::agent::llm_error_code::timeout) ||
        code == std::make_error_code(std::errc::timed_out)) {
      return reasoning_error_code::timeout;
    }
    if (code ==
        ::wuwe::agent::make_error_code(
          ::wuwe::agent::llm_error_code::agent_loop_budget_exceeded)) {
      return reasoning_error_code::tool_round_budget_exceeded;
    }
    if (code == ::wuwe::agent::make_error_code(::wuwe::agent::llm_error_code::transport_error) ||
        code == ::wuwe::agent::make_error_code(::wuwe::agent::llm_error_code::http_error) ||
        code == ::wuwe::agent::make_error_code(::wuwe::agent::llm_error_code::api_error) ||
        code == ::wuwe::agent::make_error_code(::wuwe::agent::llm_error_code::invalid_response) ||
        code == ::wuwe::agent::make_error_code(::wuwe::agent::llm_error_code::empty_response)) {
      return reasoning_error_code::transport_failed;
    }
    return reasoning_error_code::unknown;
  }

  static std::optional<std::size_t> metadata_size(
    const std::map<std::string, std::string>& metadata,
    const std::string& key) {
    const auto it = metadata.find(key);
    if (it == metadata.end()) {
      return std::nullopt;
    }
    try {
      return static_cast<std::size_t>(std::stoull(it->second));
    }
    catch (...) {
      return std::nullopt;
    }
  }

  static reasoning_error_code reasoning_error_from_plan_stop(
    planning::plan_run_stop_reason reason) {
    switch (reason) {
      case planning::plan_run_stop_reason::cancelled:
        return reasoning_error_code::cancelled;
      case planning::plan_run_stop_reason::step_budget_exhausted:
      case planning::plan_run_stop_reason::max_iterations:
        return reasoning_error_code::planning_budget_exceeded;
      default:
        return reasoning_error_code::planning_failed;
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
      .reasoning_delta = event.reasoning_delta,
      .reasoning_summary = event.reasoning_summary,
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

inline std::shared_ptr<reflection::reflection_runner> make_default_reflection_runner() {
  return std::make_shared<reflection::reflection_runner>(
    reflection::reflection_runner_options {
      .reflector = std::make_shared<reflection::rule_reflector>(),
    });
}

inline std::shared_ptr<planning::planner> make_default_planner(
  llm_client& client,
  const default_agentic_runner_options& options) {
  if (options.planner) {
    return options.planner;
  }
  if (!options.model.empty()) {
    return std::make_shared<planning::llm_planner>(
      client,
      planning::llm_planner_options {
        .model = options.model,
      });
  }
  return std::make_shared<planning::static_planner>();
}

inline planning::plan_step_result complete_passthrough_plan_step(
  const planning::plan_step& step) {
  if (!step.input.empty()) {
    return planning::plan_step_result::completed(step.input);
  }
  if (!step.description.empty()) {
    return planning::plan_step_result::completed(step.description);
  }
  return planning::plan_step_result::completed(step.title);
}

inline std::shared_ptr<planning::plan_executor> make_default_passthrough_executor() {
  return std::make_shared<planning::function_plan_executor>(
    [](const planning::plan_step& step, const planning::plan_execution_context&) {
      return complete_passthrough_plan_step(step);
    });
}

template<typename ToolProvider>
std::shared_ptr<planning::plan_executor> make_default_tool_or_passthrough_executor(
  std::shared_ptr<ToolProvider> provider) {
  auto tool_executor = std::make_shared<planning::tool_plan_executor>(std::move(provider));
  return std::make_shared<planning::function_plan_executor>(
    [tool_executor](const planning::plan_step& step, const planning::plan_execution_context& context) {
      if (step.assigned_tool && !step.assigned_tool->empty()) {
        return tool_executor->execute(step, context);
      }
      return complete_passthrough_plan_step(step);
    });
}

template<typename ToolProvider>
reasoning_runner make_default_agentic_runner(
  llm_client& client,
  std::shared_ptr<ToolProvider> provider,
  default_agentic_runner_options options = {}) {
  auto executor = options.executor;
  if (!executor) {
    executor = make_default_tool_or_passthrough_executor(provider);
  }

  reasoning_runner_options runner_options {
    .client = &client,
    .planner = make_default_planner(client, options),
    .executor = std::move(executor),
    .plan_store = options.plan_store,
    .memory = options.memory,
    .available_tools = [provider] {
      return provider->tools();
    },
    .reflection = options.reflection ? options.reflection : make_default_reflection_runner(),
    .observer = std::move(options.observer),
    .should_cancel = std::move(options.should_cancel),
  };
  return reasoning_runner::with_tools(client, std::move(provider), std::move(runner_options));
}

inline reasoning_runner make_default_agentic_runner(
  llm_client& client,
  default_agentic_runner_options options = {}) {
  auto executor = options.executor;
  if (!executor) {
    executor = make_default_passthrough_executor();
  }

  return reasoning_runner({
    .client = &client,
    .planner = make_default_planner(client, options),
    .executor = std::move(executor),
    .plan_store = options.plan_store,
    .memory = options.memory,
    .reflection = options.reflection ? options.reflection : make_default_reflection_runner(),
    .observer = std::move(options.observer),
    .should_cancel = std::move(options.should_cancel),
  });
}

inline reasoning_runner make_knowledge_aware_runner(
  llm_client& client,
  knowledge::knowledge_retriever& retriever,
  default_agentic_runner_options options = {},
  knowledge::knowledge_tool_options knowledge_options = {}) {
  auto provider = std::make_shared<knowledge::knowledge_tool_provider>(
    retriever,
    std::move(knowledge_options));
  return make_default_agentic_runner(client, std::move(provider), std::move(options));
}

} // namespace wuwe::agent::reasoning

#endif // WUWE_AGENT_REASONING_RUNNER_HPP
