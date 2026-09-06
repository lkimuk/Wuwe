#ifndef WUWE_AGENT_LLM_AGENT_RUNNER_H
#define WUWE_AGENT_LLM_AGENT_RUNNER_H

#include <algorithm>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <wuwe/agent/approval/approval_context.hpp>
#include <wuwe/agent/approval/approval_service.hpp>
#include <wuwe/agent/core/execution_context.hpp>
#include <wuwe/agent/llm/context_budget.hpp>
#include <wuwe/agent/llm/llm_client.h>
#include <wuwe/agent/llm/llm_error.h>
#include <wuwe/agent/llm/llm_usage.hpp>
#include <wuwe/agent/llm/tool_output_projection.hpp>
#include <wuwe/agent/memory/memory_context.hpp>
#include <wuwe/agent/runtime/runtime.hpp>
#include <wuwe/agent/tools/json_schema.hpp>
#include <wuwe/agent/tools/tool.hpp>
#include <wuwe/common/sha256.hpp>
#include <wuwe/common/wuwe_fwd.h>

WUWE_NAMESPACE_BEGIN

enum class llm_agent_event_type {
  model_started,
  model_first_event,
  model_content_delta,
  model_reasoning_delta,
  model_reasoning_completed,
  tool_call_building,
  tool_call_ready,
  tool_started,
  tool_heartbeat,
  tool_completed,
  tool_approval_required,
  agent_resumed,
  model_completed,
  agent_completed,
  agent_failed,
  agent_cancelled,
  tool_output_truncated,
};

enum class llm_tool_authorization_kind {
  allow,
  deny,
  suspend,
};

struct llm_tool_authorization {
  llm_tool_authorization_kind kind { llm_tool_authorization_kind::allow };
  std::string reason;
  std::map<std::string, std::string> metadata;
};

struct llm_agent_event {
  llm_agent_event_type type { llm_agent_event_type::model_started };
  std::string message;
  std::string delta;
  const llm_request* request {};
  const llm_stream_event* stream_event {};
  const llm_tool_call* tool_call {};
  const llm_tool_result* tool_result {};
  const llm_response* response {};
  const agent::llm::tool_output_projection_report* tool_output_projection {};
};

struct llm_agent_model_continuation {
  std::string instruction;
  std::optional<llm_tool_choice> tool_choice;
};

struct llm_agent_terminal_finalization {
  std::string instruction;
};

using llm_agent_tool_budget_finalization = llm_agent_terminal_finalization;

struct llm_agent_callbacks {
  std::function<bool(const llm_request&)> on_model_start;
  std::function<bool(const llm_tool_call&)> allow_tool_call;
  std::function<void(const llm_agent_event&)> on_event;
  std::function<void(std::string_view)> on_delta;
  std::function<void(std::string_view)> on_reasoning_delta;
  std::function<void(std::string_view)> on_reasoning_done;
  std::function<void(const llm_stream_event&)> on_stream_event;
  std::function<void(const llm_tool_call&)> on_tool_start;
  std::function<void(const llm_tool_call&, const agent::tools::tool_heartbeat&)> on_tool_heartbeat;
  std::function<void(const llm_tool_call&, const llm_tool_result&)> on_tool_result;
  std::function<void(const llm_response&)> on_done;
  std::function<void(std::error_code, std::string_view)> on_error;
  std::function<void()> on_cancelled;
  std::function<std::optional<llm_tool_call>(const llm_tool_call&)> prepare_tool_call;
  std::function<std::optional<llm_tool_result>(const llm_tool_call&, llm_tool_result)>
    prepare_tool_result;
  std::function<std::optional<llm_request>(llm_request)> prepare_model_request;
  std::function<void(const llm_request&, const llm_response&)> on_model_result;
  std::function<std::optional<llm_agent_model_continuation>(
    const llm_request&, const llm_response&, std::size_t)> continue_after_model_response;
  std::function<std::optional<llm_agent_terminal_finalization>(
    const llm_request&, const llm_response&, std::size_t, std::size_t)>
    finalize_after_model_continuation_budget;
  std::function<std::optional<llm_agent_tool_budget_finalization>(
    const llm_request&, const llm_response&, std::size_t, std::size_t)>
    finalize_after_tool_round_budget;
  std::function<bool(const llm_request&, const llm_response&, std::size_t,
    std::size_t, std::size_t)> continue_after_tool_round_window;
  std::function<void(const agent::llm::context_budget_report&)> on_context_budget;
  std::function<llm_tool_authorization(const agent::tools::tool_invocation&)> authorize_tool_call;
  std::function<void(const llm_tool_call&, const agent::llm::tool_output_projection_report&)>
    on_tool_output_projection;
};

struct llm_agent_run_options {
  std::stop_token stop_token;
  agent::core::agent_execution_context context;
  std::shared_ptr<agent::runtime::agent_run_runtime> runtime;
  std::shared_ptr<agent::approval::approval_service> approval_service;
  std::optional<agent::llm::llm_pricing> pricing;
  std::shared_ptr<agent::runtime::executor> run_executor;
  std::shared_ptr<agent::runtime::executor> tool_executor;
  std::shared_ptr<agent::runtime::scheduler> scheduler;
  std::shared_ptr<const agent::llm::context_token_estimator> token_estimator;
  std::size_t max_in_flight_tool_invocations { 16 };
  std::size_t max_model_continuations { 0 };
  // Tool-round limits are progress windows when the host explicitly opts in.
  // This independent cap keeps the total number of windows finite.
  std::size_t max_tool_round_windows { 1 };
  llm_agent_callbacks callbacks;
  bool persist_request_messages { true };
  bool persist_assistant_messages { true };
  bool persist_tool_messages { true };
  std::optional<agent::llm::tool_output_projection_policy> tool_output_projection;
};

class llm_agent_run {
public:
  llm_agent_run() = default;

  llm_agent_run(agent::runtime::scheduled_task task, std::future<llm_response> future)
      : task_(std::move(task)), future_(std::move(future)) {
  }

  llm_agent_run(const llm_agent_run&) = delete;
  llm_agent_run& operator=(const llm_agent_run&) = delete;
  llm_agent_run(llm_agent_run&&) noexcept = default;
  llm_agent_run& operator=(llm_agent_run&& other) noexcept {
    if (this != &other) {
      stop_and_wait();
      task_ = std::move(other.task_);
      future_ = std::move(other.future_);
    }
    return *this;
  }

  ~llm_agent_run() {
    stop_and_wait();
  }

  bool valid() const noexcept {
    return future_.valid();
  }

  void request_stop() {
    task_.request_stop();
  }

  void wait() const {
    task_.wait();
  }

  llm_response get() {
    task_.wait();
    return future_.get();
  }

private:
  void stop_and_wait() noexcept {
    if (!task_.valid())
      return;
    task_.request_stop();
    if (task_.running_on_current_thread())
      return;
    try {
      task_.wait();
    }
    catch (...) {
      // Destruction is a cancellation barrier, not an exception boundary.
    }
  }

  agent::runtime::scheduled_task task_;
  std::future<llm_response> future_;
};

class llm_agent_runner {
public:
  explicit llm_agent_runner(llm_client& client, int max_tool_rounds = 4)
      : client_(client), max_tool_rounds_(max_tool_rounds) {
    validate_max_tool_rounds();
  }

  explicit llm_agent_runner(
    llm_client& client, agent::memory::memory_context* memory, int max_tool_rounds = 4)
      : client_(client), memory_(memory), max_tool_rounds_(max_tool_rounds) {
    validate_max_tool_rounds();
  }

  template<typename ToolProvider>
  explicit llm_agent_runner(
    llm_client& client, std::shared_ptr<ToolProvider> tool_provider, int max_tool_rounds = 4)
      : client_(client), tools_([tool_provider] { return tool_provider->tools(); }),
        max_tool_rounds_(max_tool_rounds) {
    if (!tool_provider) {
      throw std::invalid_argument("llm_agent_runner requires a tool provider");
    }
    validate_max_tool_rounds();
    bind_invoke(std::move(tool_provider));
  }

  template<typename ToolProvider>
  explicit llm_agent_runner(llm_client& client, std::shared_ptr<ToolProvider> tool_provider,
    agent::memory::memory_context* memory, int max_tool_rounds = 4)
      : client_(client), tools_([tool_provider] { return tool_provider->tools(); }),
        memory_(memory), max_tool_rounds_(max_tool_rounds) {
    if (!tool_provider) {
      throw std::invalid_argument("llm_agent_runner requires a tool provider");
    }
    validate_max_tool_rounds();
    bind_invoke(std::move(tool_provider));
  }

  llm_response complete(std::string_view prompt) const {
    llm_request request;
    request.messages.push_back({ .role = "user", .content = std::string(prompt) });
    return complete(std::move(request), {});
  }

  llm_response complete(std::string_view prompt, llm_agent_run_options options) const {
    llm_request request;
    request.messages.push_back({ .role = "user", .content = std::string(prompt) });
    return complete(std::move(request), std::move(options));
  }

  llm_response complete(llm_request request) const {
    return complete(std::move(request), {});
  }

  llm_response complete(llm_request request, llm_agent_run_options options) const {
    const auto stop_token = options.stop_token;
    const auto is_cancelled = [stop_token] { return stop_token.stop_requested(); };
    return complete_impl(std::move(request), std::move(options), stop_token, is_cancelled);
  }

  llm_response resume(const std::string& run_id, std::uint64_t expected_revision,
    const std::string& continuation_token, llm_agent_run_options options = {}) const {
    validate_run_options(options);
    if (!options.runtime) {
      throw std::invalid_argument("agent resume requires a durable runtime");
    }
    const auto stored = options.runtime->get(run_id);
    if (!stored) {
      throw std::invalid_argument("agent run not found: " + run_id);
    }
    const auto stored_continuation =
      stored->suspension ? stored->suspension : stored->active_continuation;
    if (!stored_continuation) {
      throw std::logic_error("agent run has no suspended continuation");
    }
    auto continuation =
      agent::runtime::llm_continuation_from_json(stored_continuation->continuation);
    if (!options.pricing) {
      options.pricing = continuation.pricing;
    }
    if (options.tool_output_projection) {
      continuation.tool_output_projection = agent::llm::tighten_tool_output_projection_policy(
        continuation.tool_output_projection, *options.tool_output_projection);
    }
    options.tool_output_projection = continuation.tool_output_projection;
    validate_run_options(options);
    const auto claim =
      options.runtime->claim_approved_continuation(run_id, expected_revision, continuation_token);
    if (!claim) {
      llm_response response {
        .content = "Agent run changed before its continuation could be claimed.",
        .error_code = agent::make_error_code(agent::llm_error_code::run_state_conflict),
      };
      response.metadata["run_id"] = run_id;
      response.metadata["actual_revision"] = std::to_string(claim.revision);
      return response;
    }

    options.context = stored->context;
    options.context.run_id = run_id;
    options.context.stop_token = options.stop_token;
    if (std::find(continuation.approved_call_ids.begin(),
          continuation.approved_call_ids.end(),
          claim.continuation->tool_call_id) == continuation.approved_call_ids.end()) {
      continuation.approved_call_ids.push_back(claim.continuation->tool_call_id);
    }
    const auto stop_token = options.stop_token;
    const auto is_cancelled = [stop_token] { return stop_token.stop_requested(); };
    return resume_impl(continuation, std::move(options), stop_token, is_cancelled, claim.revision);
  }

  llm_agent_run run_async(llm_request request, llm_agent_run_options options = {}) const {
    validate_run_options(options);
    auto promise = std::make_shared<std::promise<llm_response>>();
    auto future = promise->get_future();
    auto runner = *this;
    auto run_executor =
      options.run_executor ? options.run_executor : agent::runtime::default_executor();
    if (options.tool_executor &&
        run_executor->execution_domain() == options.tool_executor->execution_domain()) {
      throw std::invalid_argument(
        "agent run and tool execution require independent executor domains");
    }
    // The selected executor is retained by the submitted closure. Keeping it
    // out of options prevents accidental duplicate ownership and makes a
    // temporary injected executor safe for asynchronous use.
    options.run_executor.reset();
    auto task = run_executor->submit(
      [runner = std::move(runner),
        request = std::move(request),
        options = std::move(options),
        promise,
        executor_keep_alive = run_executor](std::stop_token task_stop_token) mutable {
        const auto external_stop_token = options.stop_token;
        std::stop_source run_stop_source;
        std::stop_callback external_stop_callback(
          external_stop_token, [&run_stop_source] { run_stop_source.request_stop(); });
        std::stop_callback task_stop_callback(
          task_stop_token, [&run_stop_source] { run_stop_source.request_stop(); });

        if (external_stop_token.stop_requested() || task_stop_token.stop_requested()) {
          run_stop_source.request_stop();
        }

        const auto run_stop_token = run_stop_source.get_token();
        const auto is_cancelled = [run_stop_token] { return run_stop_token.stop_requested(); };

        try {
          promise->set_value(runner.complete_impl(
            std::move(request), std::move(options), run_stop_token, is_cancelled));
        }
        catch (...) {
          const auto failure = std::current_exception();
          try {
            promise->set_exception(failure);
          }
          catch (...) {
            // Never allow promise bookkeeping to escape a worker thread.
          }
        }
      });

    return llm_agent_run(std::move(task), std::move(future));
  }

  llm_agent_run run_async(std::string_view prompt, llm_agent_run_options options = {}) const {
    llm_request request;
    request.messages.push_back({ .role = "user", .content = std::string(prompt) });
    return run_async(std::move(request), std::move(options));
  }

private:
  void validate_max_tool_rounds() const {
    if (max_tool_rounds_ < 0) {
      throw std::invalid_argument("llm_agent_runner max_tool_rounds must not be negative");
    }
  }

  template<typename ToolProvider>
  void bind_invoke(std::shared_ptr<ToolProvider> tool_provider) {
    tools_ = [tool_provider] {
      if constexpr (requires { tool_provider->descriptors(); }) {
        std::vector<llm_tool> output;
        for (const auto& descriptor : tool_provider->descriptors()) {
          output.push_back(descriptor.model_tool());
        }
        return output;
      }
      else {
        return tool_provider->tools();
      }
    };
    descriptors_ = [tool_provider] {
      if constexpr (requires { tool_provider->descriptors(); }) {
        return tool_provider->descriptors();
      }
      else {
        std::vector<agent::tools::tool_descriptor> output;
        for (const auto& tool : tool_provider->tools()) {
          output.push_back(agent::tools::descriptor_from_llm_tool(tool));
        }
        return output;
      }
    };
    invoke_ = [tool_provider](const agent::tools::tool_invocation& invocation) {
      if constexpr (requires { tool_provider->invoke(invocation); }) {
        return tool_provider->invoke(invocation);
      }
      else if constexpr (requires {
                           tool_provider->invoke(
                             invocation.name, invocation.arguments_json, invocation.stop_token);
                         }) {
        return tool_provider->invoke(
          invocation.name, invocation.arguments_json, invocation.stop_token);
      }
      else {
        return tool_provider->invoke(invocation.name, invocation.arguments_json);
      }
    };
    provider_capabilities_ = [tool_provider](const std::string& name) {
      return agent::tools::resolve_tool_provider_capabilities(*tool_provider, name);
    };
    if constexpr (requires(const agent::tools::tool_invocation& invocation,
                    const llm_tool_result& outcome) {
                    {
                      tool_provider->compensate(invocation, outcome)
                      } -> std::convertible_to<llm_tool_result>;
                  }) {
      compensate_ = [tool_provider](const agent::tools::tool_invocation& invocation,
                      const llm_tool_result& outcome) {
        return tool_provider->compensate(invocation, outcome);
      };
    }
  }

  struct durable_run_state {
    std::shared_ptr<agent::runtime::agent_run_runtime> runtime;
    std::string run_id;
    std::uint64_t revision { 0 };
    std::optional<agent::llm::llm_pricing> pricing;
  };

  llm_response complete_impl(llm_request request, llm_agent_run_options options,
    std::stop_token client_stop_token, const std::function<bool()>& is_cancelled) const {
    if (!options.tool_output_projection) {
      options.tool_output_projection = agent::llm::tool_output_projection_policy {};
    }
    validate_run_options(options);
    if (tools_) {
      request.tools = tools_();
    }
    if (auto rejected = agent::llm::llm_request_rejection(request, client_.capabilities())) {
      emit_error(options.callbacks, *rejected);
      return std::move(*rejected);
    }
    options.context.stop_token = client_stop_token;
    auto durable = begin_durable_run(options);
    request.execution_context = options.context;
    if (is_cancelled()) {
      return finalize_durable_run(cancelled_response(options.callbacks), durable);
    }
    if (options.context.deadline_reached()) {
      return finalize_durable_run(timeout_response(options.callbacks), durable);
    }

    const std::string query_text = last_user_content(request);
    const llm_request request_to_observe = request;
    if (memory_) {
      request = memory_->augment(std::move(request),
        query_text,
        agent::memory::memory_scope_from_execution_context(options.context, memory_->scope()));
      if (options.persist_request_messages) {
        observe_request_messages(request_to_observe, options.context);
      }
    }

    const bool use_streaming = should_stream(options.callbacks);
    auto response = complete_model(request, options, client_stop_token, use_streaming);
    llm_usage accumulated_usage;
    agent::llm::accumulate_llm_usage(accumulated_usage, response.usage);
    if (is_cancelled() || response.error_code == agent::llm_error_code::cancelled) {
      auto cancelled = cancelled_response(options.callbacks);
      cancelled.usage = accumulated_usage;
      return finalize_durable_run(std::move(cancelled), durable);
    }
    if (response.error_code) {
      emit_error(options.callbacks, response);
      return finalize_durable_run(std::move(response), durable);
    }
    emit_nonstreaming_content(options.callbacks, response, use_streaming);
    return continue_tool_loop(std::move(request),
      std::move(response),
      0,
      accumulated_usage,
      std::move(options),
      client_stop_token,
      is_cancelled,
      durable);
  }

  llm_response resume_impl(agent::runtime::llm_tool_continuation continuation,
    llm_agent_run_options options, std::stop_token client_stop_token,
    const std::function<bool()>& is_cancelled, std::uint64_t revision) const {
    validate_run_options(options);
    durable_run_state durable {
      .runtime = options.runtime,
      .run_id = options.context.run_id,
      .revision = revision,
      .pricing = options.pricing,
    };
    if (auto rejected =
          agent::llm::llm_request_rejection(continuation.request, client_.capabilities())) {
      emit_error(options.callbacks, *rejected);
      return finalize_durable_run(std::move(*rejected), durable);
    }
    continuation.request.execution_context = options.context;
    emit_agent_event(options.callbacks,
      {
        .type = llm_agent_event_type::agent_resumed,
        .message = durable.run_id,
      });
    if (is_cancelled()) {
      auto cancelled = cancelled_response(options.callbacks);
      cancelled.usage = continuation.accumulated_usage;
      return finalize_durable_run(std::move(cancelled), durable);
    }
    if (options.context.deadline_reached()) {
      auto timed_out = timeout_response(options.callbacks);
      timed_out.usage = continuation.accumulated_usage;
      return finalize_durable_run(std::move(timed_out), durable);
    }

    const bool use_streaming = should_stream(options.callbacks);
    llm_response batch_response;
    if (auto terminal = process_tool_batch(continuation.request,
          continuation.pending_calls,
          continuation.used_tool_rounds,
          std::string {},
          std::string {},
          true,
          continuation.assistant_persisted,
          continuation.accumulated_usage,
          continuation.approved_call_ids,
          options,
          client_stop_token,
          is_cancelled,
          durable)) {
      terminal->usage = continuation.accumulated_usage;
      return finalize_durable_run(std::move(*terminal), durable);
    }

    batch_response =
      complete_model(continuation.request, options, client_stop_token, use_streaming);
    agent::llm::accumulate_llm_usage(continuation.accumulated_usage, batch_response.usage);
    batch_response.usage = continuation.accumulated_usage;
    if (is_cancelled() || batch_response.error_code == agent::llm_error_code::cancelled) {
      auto cancelled = cancelled_response(options.callbacks);
      cancelled.usage = continuation.accumulated_usage;
      return finalize_durable_run(std::move(cancelled), durable);
    }
    if (batch_response.error_code) {
      emit_error(options.callbacks, batch_response);
      return finalize_durable_run(std::move(batch_response), durable);
    }
    emit_nonstreaming_content(options.callbacks, batch_response, use_streaming);
    return continue_tool_loop(std::move(continuation.request),
      std::move(batch_response),
      continuation.used_tool_rounds,
      continuation.accumulated_usage,
      std::move(options),
      client_stop_token,
      is_cancelled,
      durable);
  }

  llm_response continue_tool_loop(llm_request request, llm_response response, int used_tool_rounds,
    llm_usage accumulated_usage, llm_agent_run_options options, std::stop_token client_stop_token,
    const std::function<bool()>& is_cancelled, durable_run_state& durable) const {
    llm_tool_call last_tool_call;
    std::string last_tool_projection;
    std::set<std::string> seen_tool_call_ids;
    std::size_t used_model_continuations = 0;
    std::size_t tool_rounds_in_window = 0;
    std::size_t completed_tool_round_windows = 0;
    const bool use_streaming = should_stream(options.callbacks);

    while (true) {
      if (is_cancelled()) {
        auto cancelled = cancelled_response(options.callbacks);
        cancelled.usage = accumulated_usage;
        return finalize_durable_run(std::move(cancelled), durable);
      }
      if (options.context.deadline_reached()) {
        auto timed_out = timeout_response(options.callbacks);
        timed_out.usage = accumulated_usage;
        return finalize_durable_run(std::move(timed_out), durable);
      }
      if (response.tool_calls.empty()) {
        const auto continuation = options.callbacks.continue_after_model_response
          ? options.callbacks.continue_after_model_response(
              request, response, used_model_continuations)
          : std::optional<llm_agent_model_continuation> {};
        if (continuation) {
          if (continuation->instruction.empty()) {
            response.usage = accumulated_usage;
            response.error_code =
              agent::make_error_code(agent::llm_error_code::invalid_request);
            response.stop_reason = "empty_model_continuation_instruction";
            response.metadata["stop_reason"] = response.stop_reason;
            response.content =
              "Agent model continuation callback returned an empty instruction.";
            emit_error(options.callbacks, response);
            return finalize_durable_run(std::move(response), durable);
          }
          if (used_model_continuations >= options.max_model_continuations) {
            const auto finalization = options.callbacks.finalize_after_model_continuation_budget
              ? options.callbacks.finalize_after_model_continuation_budget(request,
                  response,
                  used_model_continuations,
                  options.max_model_continuations)
              : std::optional<llm_agent_terminal_finalization> {};
            if (finalization && !finalization->instruction.empty()) {
              request.messages.push_back({
                .role = "assistant",
                .content = response.content,
                .reasoning_content = response.reasoning_summary,
              });
              request.messages.push_back({
                .role = "user",
                .content = finalization->instruction,
              });
              request.tools.clear();
              request.tool_choice.reset();
              auto final_options = options;
              const auto prepare_final_request =
                final_options.callbacks.prepare_model_request;
              final_options.callbacks.prepare_model_request =
                [prepare_final_request](llm_request final_request)
                  -> std::optional<llm_request> {
                if (prepare_final_request) {
                  auto prepared = prepare_final_request(std::move(final_request));
                  if (!prepared) {
                    return std::nullopt;
                  }
                  final_request = std::move(*prepared);
                }
                final_request.tools.clear();
                final_request.tool_choice.reset();
                return final_request;
              };
              auto final_response = complete_model(
                request, final_options, client_stop_token, use_streaming);
              agent::llm::accumulate_llm_usage(accumulated_usage, final_response.usage);
              final_response.usage = accumulated_usage;
              final_response.metadata["used_model_continuations"] =
                std::to_string(used_model_continuations);
              final_response.metadata["max_model_continuations"] =
                std::to_string(options.max_model_continuations);
              final_response.metadata["model_continuation_budget_finalization"] = "true";
              if (is_cancelled() ||
                  final_response.error_code == agent::llm_error_code::cancelled) {
                auto cancelled = cancelled_response(options.callbacks);
                cancelled.usage = accumulated_usage;
                return finalize_durable_run(std::move(cancelled), durable);
              }
              if (final_response.error_code) {
                emit_error(options.callbacks, final_response);
                return finalize_durable_run(std::move(final_response), durable);
              }
              if (!final_response.tool_calls.empty()) {
                final_response.error_code =
                  agent::make_error_code(agent::llm_error_code::invalid_response);
                final_response.stop_reason =
                  "model_continuation_finalization_requested_tool";
                final_response.content =
                  "Model-continuation finalization returned a tool call after tools were disabled.";
                emit_error(options.callbacks, final_response);
                return finalize_durable_run(std::move(final_response), durable);
              }
              emit_nonstreaming_content(options.callbacks, final_response, use_streaming);
              if (options.persist_assistant_messages) {
                observe_assistant_response(final_response, nullptr, options.context);
              }
              emit_done(options.callbacks, final_response);
              return finalize_durable_run(std::move(final_response), durable);
            }
            response.usage = accumulated_usage;
            response.error_code =
              agent::make_error_code(agent::llm_error_code::agent_loop_budget_exceeded);
            response.stop_reason = "model_continuation_budget_exceeded";
            response.metadata["stop_reason"] = response.stop_reason;
            response.metadata["used_model_continuations"] =
              std::to_string(used_model_continuations);
            response.metadata["max_model_continuations"] =
              std::to_string(options.max_model_continuations);
            response.content =
              "Agent model continuation budget exceeded before producing a terminal answer.";
            emit_error(options.callbacks, response);
            return finalize_durable_run(std::move(response), durable);
          }

          request.messages.push_back({
            .role = "assistant",
            .content = response.content,
            .reasoning_content = response.reasoning_summary,
          });
          request.messages.push_back({
            .role = "user",
            .content = continuation->instruction,
          });
          request.tool_choice = continuation->tool_choice;
          ++used_model_continuations;
          response = complete_model(request, options, client_stop_token, use_streaming);
          request.tool_choice.reset();
          agent::llm::accumulate_llm_usage(accumulated_usage, response.usage);
          if (is_cancelled() || response.error_code == agent::llm_error_code::cancelled) {
            auto cancelled = cancelled_response(options.callbacks);
            cancelled.usage = accumulated_usage;
            return finalize_durable_run(std::move(cancelled), durable);
          }
          if (response.error_code) {
            response.usage = accumulated_usage;
            emit_error(options.callbacks, response);
            return finalize_durable_run(std::move(response), durable);
          }
          emit_nonstreaming_content(options.callbacks, response, use_streaming);
          continue;
        }
        response.usage = accumulated_usage;
        response.metadata["used_tool_rounds"] = std::to_string(used_tool_rounds);
        response.metadata["max_tool_rounds"] = std::to_string(max_tool_rounds_);
        response.metadata["completed_tool_round_windows"] =
          std::to_string(completed_tool_round_windows);
        response.metadata["max_tool_round_windows"] =
          std::to_string(options.max_tool_round_windows);
        if (options.persist_assistant_messages) {
          observe_assistant_response(response, nullptr, options.context);
        }
        emit_done(options.callbacks, response);
        return finalize_durable_run(std::move(response), durable);
      }
      if (tool_rounds_in_window >= static_cast<std::size_t>(max_tool_rounds_)) {
        const bool continue_window = completed_tool_round_windows + 1
            < options.max_tool_round_windows
          && options.callbacks.continue_after_tool_round_window
          && options.callbacks.continue_after_tool_round_window(request,
            response,
            tool_rounds_in_window,
            static_cast<std::size_t>(max_tool_rounds_),
            completed_tool_round_windows);
        if (continue_window) {
          tool_rounds_in_window = 0;
          ++completed_tool_round_windows;
        } else {
          const auto finalization = options.callbacks.finalize_after_tool_round_budget
            ? options.callbacks.finalize_after_tool_round_budget(request,
                response,
                static_cast<std::size_t>(used_tool_rounds),
                static_cast<std::size_t>(max_tool_rounds_))
            : std::optional<llm_agent_tool_budget_finalization> {};
          if (finalization && !finalization->instruction.empty()) {
          // The response that crossed the boundary contains unexecuted tool
          // calls and therefore cannot be replayed as an assistant message.
          // Continue from the last complete tool-result turn and remove tools
          // so the recovery call must synthesize a terminal answer.
          request.messages.push_back({
            .role = "user",
            .content = finalization->instruction,
          });
          request.tools.clear();
          request.tool_choice.reset();
          auto final_options = options;
          const auto prepare_final_request =
            final_options.callbacks.prepare_model_request;
          final_options.callbacks.prepare_model_request =
            [prepare_final_request](llm_request final_request)
              -> std::optional<llm_request> {
            if (prepare_final_request) {
              auto prepared = prepare_final_request(std::move(final_request));
              if (!prepared) {
                return std::nullopt;
              }
              final_request = std::move(*prepared);
            }
            final_request.tools.clear();
            final_request.tool_choice.reset();
            return final_request;
          };
          auto final_response =
            complete_model(request, final_options, client_stop_token, use_streaming);
          agent::llm::accumulate_llm_usage(accumulated_usage, final_response.usage);
          final_response.usage = accumulated_usage;
          final_response.metadata["used_tool_rounds"] =
            std::to_string(used_tool_rounds);
          final_response.metadata["max_tool_rounds"] =
            std::to_string(max_tool_rounds_);
          final_response.metadata["completed_tool_round_windows"] =
            std::to_string(completed_tool_round_windows);
          final_response.metadata["max_tool_round_windows"] =
            std::to_string(options.max_tool_round_windows);
          final_response.metadata["tool_round_budget_finalization"] = "true";
          if (is_cancelled() ||
              final_response.error_code == agent::llm_error_code::cancelled) {
            auto cancelled = cancelled_response(options.callbacks);
            cancelled.usage = accumulated_usage;
            return finalize_durable_run(std::move(cancelled), durable);
          }
          if (final_response.error_code) {
            emit_error(options.callbacks, final_response);
            return finalize_durable_run(std::move(final_response), durable);
          }
          if (!final_response.tool_calls.empty()) {
            final_response.error_code =
              agent::make_error_code(agent::llm_error_code::invalid_response);
            final_response.stop_reason = "tool_budget_finalization_requested_tool";
            final_response.content =
              "Tool-budget finalization returned a tool call after tools were disabled.";
            emit_error(options.callbacks, final_response);
            return finalize_durable_run(std::move(final_response), durable);
          }
          emit_nonstreaming_content(options.callbacks, final_response, use_streaming);
          if (options.persist_assistant_messages) {
            observe_assistant_response(final_response, nullptr, options.context);
          }
          emit_done(options.callbacks, final_response);
          return finalize_durable_run(std::move(final_response), durable);
          }
          response.usage = accumulated_usage;
          response.error_code =
            agent::make_error_code(agent::llm_error_code::agent_loop_budget_exceeded);
          response.stop_reason = "tool_round_budget_exceeded";
          response.metadata["stop_reason"] = response.stop_reason;
          response.metadata["used_tool_rounds"] = std::to_string(used_tool_rounds);
          response.metadata["max_tool_rounds"] = std::to_string(max_tool_rounds_);
          response.metadata["completed_tool_round_windows"] =
            std::to_string(completed_tool_round_windows);
          response.metadata["max_tool_round_windows"] =
            std::to_string(options.max_tool_round_windows);
          response.metadata["last_tool_call"] = last_tool_call.name;
          response.metadata["last_tool_call_id"] = last_tool_call.id;
          response.metadata["last_tool_arguments"] = last_tool_call.arguments_json;
          response.metadata["last_tool_result"] = last_tool_projection;
          response.metadata["last_model_response"] = response.content;
          response.content = "Agent tool round budget exceeded before producing a final answer.";
          emit_error(options.callbacks, response);
          return finalize_durable_run(std::move(response), durable);
        }
      }
      if (!invoke_) {
        response.usage = accumulated_usage;
        if (options.persist_assistant_messages) {
          observe_assistant_response(response, nullptr, options.context);
        }
        emit_done(options.callbacks, response);
        return finalize_durable_run(std::move(response), durable);
      }
      ++used_tool_rounds;
      ++tool_rounds_in_window;

      std::vector<llm_tool_call> prepared_calls;
      prepared_calls.reserve(response.tool_calls.size());
      std::set<std::string> prepared_call_ids;
      std::size_t call_index = 0;
      for (const auto& original_call : response.tool_calls) {
        if (!allow_tool_call(options.callbacks, original_call)) {
          auto denied = denied_tool_response(original_call, "tool call rejected by host callback");
          denied.usage = accumulated_usage;
          emit_error(options.callbacks, denied);
          return finalize_durable_run(std::move(denied), durable);
        }
        auto prepared_call = prepare_tool_call(options.callbacks, original_call);
        if (!prepared_call) {
          auto denied =
            denied_tool_response(original_call, "tool call rejected during preparation");
          denied.usage = accumulated_usage;
          emit_error(options.callbacks, denied);
          return finalize_durable_run(std::move(denied), durable);
        }
        if (prepared_call->name.empty()) {
          auto invalid =
            invalid_tool_call_response(*prepared_call, "model returned a tool call without a name");
          invalid.usage = accumulated_usage;
          emit_error(options.callbacks, invalid);
          return finalize_durable_run(std::move(invalid), durable);
        }
        if (prepared_call->id.empty()) {
          prepared_call->id =
            "wuwe-call-" + std::to_string(used_tool_rounds) + "-" + std::to_string(call_index);
        }
        if (!prepared_call_ids.insert(prepared_call->id).second) {
          auto invalid =
            invalid_tool_call_response(*prepared_call, "model returned duplicate tool call ids");
          invalid.usage = accumulated_usage;
          emit_error(options.callbacks, invalid);
          return finalize_durable_run(std::move(invalid), durable);
        }
        prepared_calls.push_back(std::move(*prepared_call));
        ++call_index;
      }

      for (const auto& call : prepared_calls) {
        if (seen_tool_call_ids.contains(call.id)) {
          auto invalid =
            invalid_tool_call_response(call, "model reused a tool call id from an earlier round");
          invalid.usage = accumulated_usage;
          emit_error(options.callbacks, invalid);
          return finalize_durable_run(std::move(invalid), durable);
        }
      }
      if (auto terminal = process_tool_batch(request,
            prepared_calls,
            used_tool_rounds,
            response.content,
            response.reasoning_summary,
            false,
            false,
            accumulated_usage,
            {},
            options,
            client_stop_token,
            is_cancelled,
            durable,
            &last_tool_call,
            &last_tool_projection)) {
        terminal->usage = accumulated_usage;
        return finalize_durable_run(std::move(*terminal), durable);
      }
      for (const auto& call : prepared_calls) {
        seen_tool_call_ids.insert(call.id);
      }

      response = complete_model(request, options, client_stop_token, use_streaming);
      agent::llm::accumulate_llm_usage(accumulated_usage, response.usage);
      if (is_cancelled() || response.error_code == agent::llm_error_code::cancelled) {
        auto cancelled = cancelled_response(options.callbacks);
        cancelled.usage = accumulated_usage;
        return finalize_durable_run(std::move(cancelled), durable);
      }
      if (response.error_code) {
        response.usage = accumulated_usage;
        emit_error(options.callbacks, response);
        return finalize_durable_run(std::move(response), durable);
      }
      emit_nonstreaming_content(options.callbacks, response, use_streaming);
    }
  }

  std::optional<llm_response> process_tool_batch(llm_request& request,
    const std::vector<llm_tool_call>& calls, int used_tool_rounds,
    const std::string& assistant_content, const std::string& assistant_reasoning_content,
    bool assistant_already_in_request,
    bool assistant_persisted, const llm_usage& accumulated_usage,
    const std::vector<std::string>& approved_tool_call_ids, const llm_agent_run_options& options,
    std::stop_token client_stop_token, const std::function<bool()>& is_cancelled,
    durable_run_state& durable, llm_tool_call* last_tool_call = nullptr,
    std::string* last_tool_projection = nullptr) const {
    if (durable.runtime && !assistant_already_in_request) {
      const auto record = durable.runtime->get(durable.run_id);
      if (!record) {
        return run_state_conflict_response(durable.run_id, 0);
      }
      for (const auto& call : calls) {
        if (record->admitted_tool_results.contains(call.id)) {
          return invalid_tool_call_response(
            call, "model reused a tool call id from an earlier round");
        }
      }
    }
    for (const auto& call : calls) {
      if (std::find(approved_tool_call_ids.begin(), approved_tool_call_ids.end(), call.id) !=
          approved_tool_call_ids.end()) {
        continue;
      }
      const auto descriptor = descriptor_for(call.name, request);
      if (!descriptor) {
        return denied_tool_response(call, "tool is not registered in the active tool contract");
      }
      const auto projection_policy = agent::llm::effective_tool_output_projection_policy(
        *options.tool_output_projection, descriptor->model_output_projection);
      const auto projection_validation = agent::llm::check_tool_output_projection_policy(
        projection_policy, tool_output_estimator(options));
      if (!projection_validation) {
        auto failed = tool_output_projection_failure_response(call, projection_validation.error);
        emit_error(options.callbacks, failed);
        return failed;
      }
      const agent::tools::tool_invocation invocation {
        .call_id = call.id,
        .name = call.name,
        .arguments_json = call.arguments_json,
        .idempotency_key = make_idempotency_key(options.context, call),
        .descriptor = *descriptor,
        .context = options.context,
        .stop_token = client_stop_token,
      };
      const auto authorization = authorize_tool(invocation, options);
      if (authorization.kind == llm_tool_authorization_kind::deny) {
        auto denied = denied_tool_response(call, authorization.reason);
        emit_error(options.callbacks, denied);
        return denied;
      }
      if (authorization.kind == llm_tool_authorization_kind::suspend) {
        if (!durable.runtime) {
          auto denied = denied_tool_response(call, "tool approval requires a durable runtime");
          emit_error(options.callbacks, denied);
          return denied;
        }
        if (!assistant_already_in_request) {
          request.messages.push_back({
            .role = "assistant",
            .content = assistant_content,
            .reasoning_content = assistant_reasoning_content,
            .tool_calls = calls,
            .context_source = llm_context_source::tool_result,
          });
          assistant_already_in_request = true;
        }
        agent::runtime::agent_run_suspension suspension {
          .approval_id = agent::runtime::agent_run_runtime::make_identifier("approval"),
          .continuation_token = agent::runtime::agent_run_runtime::make_secret_token(),
          .tool_call_id = call.id,
          .tool_name = call.name,
          .reason =
            authorization.reason.empty() ? "tool call requires approval" : authorization.reason,
          .continuation = agent::runtime::llm_continuation_to_json({
            .request = request,
            .pending_calls = calls,
            .approved_call_ids = approved_tool_call_ids,
            .used_tool_rounds = used_tool_rounds,
            .assistant_persisted = assistant_persisted,
            .accumulated_usage = accumulated_usage,
            .pricing = durable.pricing,
            .tool_output_projection = *options.tool_output_projection,
          }),
          .metadata = authorization.metadata,
        };
        const auto write =
          durable.runtime->suspend_for_approval(durable.run_id, durable.revision, suspension);
        if (!write) {
          return run_state_conflict_response(durable.run_id, write.revision);
        }
        durable.revision = write.revision;
        emit_agent_event(options.callbacks,
          {
            .type = llm_agent_event_type::tool_approval_required,
            .message = suspension.reason,
            .tool_call = &call,
          });
        llm_response response {
          .content = suspension.reason,
          .error_code = agent::make_error_code(agent::llm_error_code::approval_required),
          .stop_reason = "approval_required",
        };
        response.metadata["run_id"] = durable.run_id;
        response.metadata["revision"] = std::to_string(durable.revision);
        response.metadata["approval_id"] = suspension.approval_id;
        response.metadata["continuation_token"] = suspension.continuation_token;
        response.metadata["tool_call_id"] = call.id;
        response.metadata["tool_name"] = call.name;
        return response;
      }
    }

    if (!assistant_already_in_request) {
      request.messages.push_back({
        .role = "assistant",
        .content = assistant_content,
        .reasoning_content = assistant_reasoning_content,
        .tool_calls = calls,
        .context_source = llm_context_source::tool_result,
      });
    }
    if (!assistant_persisted && options.persist_assistant_messages) {
      llm_response assistant_response {
        .content = assistant_content,
        .reasoning_summary = assistant_reasoning_content,
        .tool_calls = calls,
      };
      if (assistant_response.content.empty() && !request.messages.empty()) {
        assistant_response.content = request.messages.back().content;
      }
      observe_assistant_response(assistant_response, &calls, options.context);
    }

    for (const auto& call : calls) {
      if (is_cancelled()) {
        auto cancelled = cancelled_response(options.callbacks);
        cancelled.usage = accumulated_usage;
        return cancelled;
      }
      if (options.context.deadline_reached()) {
        auto timed_out = timeout_response(options.callbacks);
        timed_out.usage = accumulated_usage;
        return timed_out;
      }
      const auto descriptor = descriptor_for(call.name, request);
      if (!descriptor) {
        return denied_tool_response(call, "tool is not registered in the active tool contract");
      }
      const auto idempotency_key = make_idempotency_key(options.context, call);
      llm_tool_result tool_result;
      bool reused_result = false;
      if (durable.runtime) {
        const auto record = durable.runtime->get(durable.run_id);
        if (!record) {
          return run_state_conflict_response(durable.run_id, 0);
        }
        const auto admitted = record->admitted_tool_results.find(call.id);
        if (admitted != record->admitted_tool_results.end()) {
          tool_result = admitted->second.outcome;
          reused_result = true;
        }
      }

      if (!reused_result) {
        emit_tool_start(options.callbacks, call);
        auto invocation_context = options.context;
        if (descriptor->timeout > std::chrono::milliseconds::zero()) {
          const auto tool_deadline = std::chrono::system_clock::now() + descriptor->timeout;
          if (!invocation_context.deadline || tool_deadline < *invocation_context.deadline) {
            invocation_context.deadline = tool_deadline;
          }
        }
        const agent::tools::tool_invocation invocation {
          .call_id = call.id,
          .name = call.name,
          .arguments_json = call.arguments_json,
          .idempotency_key = idempotency_key,
          .descriptor = *descriptor,
          .context = invocation_context,
          .stop_token = client_stop_token,
        };
        tool_result = invoke_tool(invocation, options);
        normalize_tool_outcome(tool_result);
        auto prepared_result = prepare_tool_result(options.callbacks, call, std::move(tool_result));
        if (!prepared_result) {
          return denied_tool_response(call, "tool result rejected during preparation");
        }
        tool_result = std::move(*prepared_result);
        normalize_tool_outcome(tool_result);
        agent::tools::json_schema_validator output_validator;
        validate_output(tool_result, *descriptor, output_validator);
        populate_resource_version(tool_result, *descriptor);

        if (durable.runtime) {
          auto admission = durable.runtime->admit_tool_result(durable.run_id,
            durable.revision,
            {
              .tool_call_id = call.id,
              .idempotency_key = idempotency_key,
              .tool_name = call.name,
              .outcome = tool_result,
            });
          if (!admission) {
            return run_state_conflict_response(durable.run_id, admission.revision);
          }
          durable.revision = admission.revision;
          tool_result = admission.result->outcome;
        }
      }

      if (last_tool_call) {
        *last_tool_call = call;
      }
      emit_tool_result(options.callbacks, call, tool_result);
      const auto projection_policy = agent::llm::effective_tool_output_projection_policy(
        *options.tool_output_projection, descriptor->model_output_projection);
      auto projection_result = agent::llm::try_project_tool_output_for_model(
        tool_result, projection_policy, tool_output_estimator(options));
      if (!projection_result) {
        auto failed = tool_output_projection_failure_response(call, projection_result.error);
        emit_error(options.callbacks, failed);
        return failed;
      }
      auto projection = std::move(projection_result.projection);
      if (durable.runtime) {
        auto recording = durable.runtime->record_tool_output_projection(durable.run_id,
          durable.revision,
          {
            .tool_call_id = call.id,
            .tool_name = call.name,
            .projected_content_sha256 = common::sha256_hex(projection.content),
            .report = projection.report,
          });
        if (!recording) {
          return run_state_conflict_response(durable.run_id, recording.revision);
        }
        durable.revision = recording.revision;
      }
      if (last_tool_projection) {
        *last_tool_projection = projection.content;
      }
      emit_tool_output_projection(options.callbacks, call, projection.report);
      chat_message tool_message {
        .role = "tool",
        .content = projection.content,
        .name = call.name,
        .tool_call_id = call.id,
        .context_source = llm_context_source::tool_result,
      };
      request.messages.push_back(tool_message);
      if (memory_ && options.persist_tool_messages) {
        memory_->observe(tool_message,
          agent::memory::memory_scope_from_execution_context(options.context, memory_->scope()));
      }
    }
    return std::nullopt;
  }

  durable_run_state begin_durable_run(llm_agent_run_options& options) const {
    durable_run_state durable {
      .pricing = options.pricing,
    };
    if (!options.runtime) {
      return durable;
    }
    auto record = options.runtime->start(options.context);
    const auto running = options.runtime->transition(
      record.id, record.revision, agent::runtime::agent_run_status::running, "run_started");
    if (!running) {
      throw std::runtime_error("failed to start durable agent run");
    }
    options.context = record.context;
    options.context.stop_token = options.stop_token;
    durable.runtime = options.runtime;
    durable.run_id = record.id;
    durable.revision = running.revision;
    return durable;
  }

  static llm_response finalize_durable_run(llm_response response, durable_run_state& durable) {
    if (durable.pricing) {
      response.cost = agent::llm::calculate_llm_cost(response.usage, *durable.pricing);
      if (response.cost) {
        response.metadata["cost_usd"] = nlohmann::json(response.cost->total_usd).dump();
      }
      else {
        response.metadata["accounting_error"] =
          "usage or pricing is invalid; cost was not calculated";
      }
    }
    if (!durable.runtime) {
      return response;
    }
    response.metadata["run_id"] = durable.run_id;
    if (response.error_code == agent::llm_error_code::approval_required) {
      response.metadata["revision"] = std::to_string(durable.revision);
      return response;
    }
    auto status = agent::runtime::agent_run_status::completed;
    std::string event = "run_completed";
    if (response.error_code == agent::llm_error_code::cancelled) {
      status = agent::runtime::agent_run_status::cancelled;
      event = "run_cancelled";
    }
    else if (response.error_code == agent::llm_error_code::timeout) {
      status = agent::runtime::agent_run_status::timed_out;
      event = "run_timed_out";
    }
    else if (response.error_code) {
      status = agent::runtime::agent_run_status::failed;
      event = "run_failed";
    }
    nlohmann::json persisted_result {
      { "content", response.content },
      { "stop_reason", response.stop_reason },
      { "metadata", response.metadata },
      { "usage",
        {
          { "prompt_tokens", response.usage.prompt_tokens },
          { "completion_tokens", response.usage.completion_tokens },
          { "total_tokens", response.usage.total_tokens },
          { "cached_prompt_tokens", response.usage.cached_prompt_tokens },
          { "reasoning_tokens", response.usage.reasoning_tokens },
        } },
    };
    persisted_result["cost"] = response.cost
      ? nlohmann::json {
          { "input_usd", response.cost->input_usd },
          { "cached_input_usd", response.cost->cached_input_usd },
          { "output_usd", response.cost->output_usd },
          { "reasoning_usd", response.cost->reasoning_usd },
          { "total_usd", response.cost->total_usd },
        }
      : nlohmann::json(nullptr);
    const auto write = durable.runtime->finish(durable.run_id,
      durable.revision,
      status,
      std::move(persisted_result),
      response.error_code ? response.error_code.message() : std::string {},
      std::move(event));
    if (!write) {
      auto conflict = run_state_conflict_response(durable.run_id, write.revision);
      conflict.usage = response.usage;
      conflict.cost = response.cost;
      conflict.metadata["original_error"] = response.error_code.message();
      conflict.metadata["original_stop_reason"] = response.stop_reason;
      if (const auto cost = response.metadata.find("cost_usd"); cost != response.metadata.end()) {
        conflict.metadata["cost_usd"] = cost->second;
      }
      if (const auto accounting_error = response.metadata.find("accounting_error");
          accounting_error != response.metadata.end()) {
        conflict.metadata["accounting_error"] = accounting_error->second;
      }
      return conflict;
    }
    durable.revision = write.revision;
    response.metadata["revision"] = std::to_string(durable.revision);
    return response;
  }

  std::optional<agent::tools::tool_descriptor> descriptor_for(
    const std::string& name, const llm_request& request) const {
    if (descriptors_) {
      for (const auto& descriptor : descriptors_()) {
        if (descriptor.name == name) {
          agent::tools::validate_tool_descriptor(descriptor);
          return descriptor;
        }
      }
      return std::nullopt;
    }
    for (const auto& tool : request.tools) {
      if (tool.name == name) {
        return agent::tools::descriptor_from_llm_tool(tool);
      }
    }
    return std::nullopt;
  }

  struct timed_tool_execution_state {
    std::mutex mutex;
    std::size_t in_flight { 0 };
  };

  llm_tool_result invoke_tool(
    const agent::tools::tool_invocation& invocation, const llm_agent_run_options& options) const {
    auto prepared = invocation;
    const auto provider_capabilities = provider_capabilities_
                                         ? provider_capabilities_(prepared.name)
                                         : agent::tools::tool_provider_capabilities {};
    if (!agent::tools::valid_tool_provider_capabilities(provider_capabilities)) {
      return tool_configuration_result(
        "tool provider has an inconsistent invocation capability declaration");
    }
    if (prepared.descriptor.heartbeat.timeout > std::chrono::milliseconds::zero() &&
        !provider_capabilities.heartbeat) {
      return tool_configuration_result("tool heartbeat requires an invocation-aware provider");
    }
    if (prepared.descriptor.compensation.enabled &&
        (!provider_capabilities.compensation || !compensate_)) {
      return tool_configuration_result(
        "tool compensation is enabled but the provider has no compensation handler");
    }
    if (prepared.descriptor.retry.max_attempts > 1 &&
        prepared.descriptor.idempotency == agent::tools::tool_idempotency::idempotent_with_key &&
        !provider_capabilities.idempotency_key) {
      return tool_configuration_result(
        "idempotency-key retries require a provider that consumes the key");
    }
    const auto arguments = nlohmann::json::parse(
      prepared.arguments_json.empty() ? "{}" : prepared.arguments_json, nullptr, false);
    if (arguments.is_discarded()) {
      return invalid_tool_contract_result("tool arguments are not valid JSON");
    }
    agent::tools::json_schema_validator validator;
    const auto input_validation = validator.validate(arguments, prepared.descriptor.input_schema);
    if (!input_validation) {
      auto result = invalid_tool_contract_result("tool arguments do not satisfy the input schema");
      result.metadata["schema_issues"] = schema_issues_json(input_validation);
      return result;
    }
    if (prepared.descriptor.resource_version.require_expected_version) {
      try {
        const auto& value = arguments.at(
          nlohmann::json::json_pointer(prepared.descriptor.resource_version.argument_json_pointer));
        if (value.is_null() || value.is_array() || value.is_object()) {
          return invalid_tool_contract_result("expected resource version must be a scalar value");
        }
        prepared.expected_resource_version =
          value.is_string() ? value.get<std::string>() : value.dump();
      }
      catch (...) {
        return invalid_tool_contract_result(
          "tool invocation requires an expected resource version");
      }
    }

    auto scheduler = options.scheduler ? options.scheduler : agent::runtime::default_scheduler();
    llm_tool_result result;
    for (std::size_t attempt = 1; attempt <= prepared.descriptor.retry.max_attempts; ++attempt) {
      prepared.attempt = attempt;
      result = execute_tool_attempt(prepared, invoke_, options, true);
      normalize_tool_outcome(result);
      result.metadata["tool_attempt"] = std::to_string(attempt);
      if (result.succeeded()) {
        return result;
      }

      bool compensated = false;
      if (result.compensation_required) {
        compensated = compensate_failure(prepared, result, options);
        result.metadata["compensation_succeeded"] = compensated ? "true" : "false";
        if (!compensated) {
          result.retryable = false;
          return result;
        }
      }
      if (attempt == prepared.descriptor.retry.max_attempts ||
          !retryable_by_policy(result, prepared.descriptor.retry) ||
          !retry_is_safe(prepared.descriptor, compensated)) {
        return result;
      }
      const auto delay = retry_delay(prepared.descriptor.retry, attempt);
      auto wait = delay;
      bool deadline_will_expire = false;
      if (const auto remaining = prepared.context.remaining_time()) {
        if (*remaining <= std::chrono::milliseconds::zero()) {
          return late_tool_result("tool retry exceeded its deadline",
            std::errc::timed_out,
            agent::tools::tool_error_category::timeout,
            true);
        }
        if (wait >= *remaining) {
          wait = *remaining;
          deadline_will_expire = true;
        }
      }
      if (!scheduler->wait_for(wait, prepared.stop_token)) {
        return {
          .content = "tool retry cancelled during backoff",
          .error_code = std::make_error_code(std::errc::operation_canceled),
          .error_category = agent::tools::tool_error_category::cancelled,
          .retryable = false,
        };
      }
      if (deadline_will_expire) {
        return late_tool_result("tool retry exceeded its deadline",
          std::errc::timed_out,
          agent::tools::tool_error_category::timeout,
          true);
      }
    }
    return result;
  }

  using tool_callable = std::function<llm_tool_result(const agent::tools::tool_invocation&)>;

  struct tool_heartbeat_state {
    std::mutex mutex;
    bool started { false };
    std::chrono::steady_clock::time_point last_activity { std::chrono::steady_clock::now() };
    std::chrono::steady_clock::time_point last_queued {};
    std::optional<agent::tools::tool_heartbeat> pending;
  };

  llm_tool_result execute_tool_attempt(const agent::tools::tool_invocation& invocation,
    const tool_callable& callable, const llm_agent_run_options& options,
    bool enable_heartbeat) const {
    {
      std::scoped_lock lock(timed_tool_execution_state_->mutex);
      if (timed_tool_execution_state_->in_flight >= options.max_in_flight_tool_invocations) {
        return {
          .content = "tool invocation capacity is exhausted",
          .error_code = std::make_error_code(std::errc::resource_unavailable_try_again),
          .error_category = agent::tools::tool_error_category::unavailable,
          .retryable = true,
          .metadata = { { "tool_execution_capacity_exhausted", "true" } },
        };
      }
      ++timed_tool_execution_state_->in_flight;
    }

    auto heartbeat = std::make_shared<tool_heartbeat_state>();
    std::stop_source tool_stop_source;
    std::stop_callback parent_stop_callback(
      invocation.stop_token, [&tool_stop_source] { tool_stop_source.request_stop(); });
    auto asynchronous_invocation = invocation;
    asynchronous_invocation.stop_token = tool_stop_source.get_token();
    asynchronous_invocation.context.stop_token = tool_stop_source.get_token();
    if (enable_heartbeat &&
        invocation.descriptor.heartbeat.timeout > std::chrono::milliseconds::zero()) {
      asynchronous_invocation.report_heartbeat =
        [heartbeat, minimum_interval = invocation.descriptor.heartbeat.minimum_interval](
          agent::tools::tool_heartbeat update) {
          const auto now = std::chrono::steady_clock::now();
          update.timestamp = now;
          std::scoped_lock lock(heartbeat->mutex);
          heartbeat->last_activity = now;
          if (!update.progress || (*update.progress >= 0.0 && *update.progress <= 1.0)) {
            if (heartbeat->last_queued.time_since_epoch().count() == 0 ||
                now - heartbeat->last_queued >= minimum_interval) {
              heartbeat->last_queued = now;
              heartbeat->pending = std::move(update);
            }
          }
        };
    }

    auto promise = std::make_shared<std::promise<llm_tool_result>>();
    auto future = promise->get_future();
    auto executor =
      options.tool_executor ? options.tool_executor : agent::runtime::default_tool_executor();
    if (executor->owns_current_thread()) {
      {
        std::scoped_lock lock(timed_tool_execution_state_->mutex);
        if (timed_tool_execution_state_->in_flight != 0) {
          --timed_tool_execution_state_->in_flight;
        }
      }
      return tool_configuration_result(
        "tool execution cannot block on its current executor domain");
    }
    agent::runtime::scheduled_task task;
    try {
      task =
        executor->submit([callable,
                           invocation = std::move(asynchronous_invocation),
                           promise,
                           executor_stop_source = tool_stop_source,
                           heartbeat,
                           state = timed_tool_execution_state_,
                           executor_keep_alive = executor](std::stop_token executor_stop) mutable {
          std::stop_callback executor_cancel(executor_stop,
            [executor_stop_source]() mutable { executor_stop_source.request_stop(); });
          {
            std::scoped_lock lock(heartbeat->mutex);
            heartbeat->started = true;
            heartbeat->last_activity = std::chrono::steady_clock::now();
          }
          llm_tool_result outcome;
          try {
            outcome = invoke_safely(callable, invocation);
          }
          catch (...) {
            outcome = {
              .content = "tool execution bookkeeping failed",
              .error_code = std::make_error_code(std::errc::io_error),
              .error_category = agent::tools::tool_error_category::internal,
            };
          }
          {
            std::scoped_lock lock(state->mutex);
            if (state->in_flight != 0)
              --state->in_flight;
          }
          try {
            promise->set_value(std::move(outcome));
          }
          catch (...) {
          }
        });
    }
    catch (const std::exception& error) {
      {
        std::scoped_lock lock(timed_tool_execution_state_->mutex);
        if (timed_tool_execution_state_->in_flight != 0) {
          --timed_tool_execution_state_->in_flight;
        }
      }
      return {
        .content = std::string("failed to schedule tool invocation: ") + error.what(),
        .error_code = std::make_error_code(std::errc::resource_unavailable_try_again),
        .error_category = agent::tools::tool_error_category::unavailable,
        .retryable = true,
      };
    }

    constexpr auto poll_interval = std::chrono::milliseconds(10);
    for (;;) {
      emit_pending_heartbeat(invocation, options.callbacks, heartbeat);
      if (invocation.stop_token.stop_requested()) {
        tool_stop_source.request_stop();
        task.request_stop();
        return late_tool_result("tool invocation cancelled",
          std::errc::operation_canceled,
          agent::tools::tool_error_category::cancelled,
          false);
      }
      if (invocation.context.deadline_reached()) {
        tool_stop_source.request_stop();
        task.request_stop();
        return late_tool_result("tool invocation exceeded its deadline",
          std::errc::timed_out,
          agent::tools::tool_error_category::timeout,
          true);
      }
      if (enable_heartbeat &&
          invocation.descriptor.heartbeat.timeout > std::chrono::milliseconds::zero()) {
        std::chrono::steady_clock::time_point last_activity;
        bool started = false;
        {
          std::scoped_lock lock(heartbeat->mutex);
          started = heartbeat->started;
          last_activity = heartbeat->last_activity;
        }
        if (started && std::chrono::steady_clock::now() - last_activity >=
                         invocation.descriptor.heartbeat.timeout) {
          tool_stop_source.request_stop();
          task.request_stop();
          auto result = late_tool_result("tool heartbeat timed out",
            std::errc::timed_out,
            agent::tools::tool_error_category::timeout,
            true);
          result.metadata["heartbeat_timeout"] = "true";
          return result;
        }
      }
      if (future.wait_for(poll_interval) == std::future_status::ready) {
        emit_pending_heartbeat(invocation, options.callbacks, heartbeat);
        try {
          return future.get();
        }
        catch (const std::exception& error) {
          return {
            .content = std::string("tool execution bookkeeping failed: ") + error.what(),
            .error_code = std::make_error_code(std::errc::io_error),
            .error_category = agent::tools::tool_error_category::internal,
          };
        }
      }
    }
  }

  static llm_tool_result invoke_safely(
    const tool_callable& callable, const agent::tools::tool_invocation& invocation) {
    if (invocation.stop_token.stop_requested()) {
      return {
        .content = "tool invocation cancelled before dispatch",
        .error_code = std::make_error_code(std::errc::operation_canceled),
        .error_category = agent::tools::tool_error_category::cancelled,
      };
    }
    try {
      return callable(invocation);
    }
    catch (const std::exception& error) {
      return {
        .content = std::string("tool invocation failed: ") + error.what(),
        .error_code = std::make_error_code(std::errc::io_error),
        .error_category = agent::tools::tool_error_category::internal,
        .metadata = { { "tool_exception", "true" } },
      };
    }
    catch (...) {
      return {
        .content = "tool invocation failed with a non-standard exception",
        .error_code = std::make_error_code(std::errc::io_error),
        .error_category = agent::tools::tool_error_category::internal,
        .metadata = { { "tool_exception", "true" } },
      };
    }
  }

  static llm_tool_result invalid_tool_contract_result(std::string message) {
    return {
      .content = std::move(message),
      .error_code = std::make_error_code(std::errc::invalid_argument),
      .error_category = agent::tools::tool_error_category::invalid_input,
    };
  }

  static llm_tool_result tool_configuration_result(std::string message) {
    return {
      .content = std::move(message),
      .error_code = std::make_error_code(std::errc::not_supported),
      .error_category = agent::tools::tool_error_category::internal,
      .metadata = { { "tool_contract_configuration_error", "true" } },
    };
  }

  static llm_tool_result late_tool_result(std::string message, std::errc error,
    agent::tools::tool_error_category category, bool retryable) {
    return {
      .content = std::move(message),
      .error_code = std::make_error_code(error),
      .error_category = category,
      .retryable = retryable,
      .metadata = { { "late_result_discarded", "true" } },
    };
  }

  static std::string schema_issues_json(
    const agent::tools::json_schema_validation_result& validation) {
    auto issues = nlohmann::json::array();
    for (const auto& issue : validation.issues) {
      issues.push_back({
        { "instance_path", issue.instance_path },
        { "schema_path", issue.schema_path },
        { "message", issue.message },
      });
    }
    return issues.dump();
  }

  static void validate_output(llm_tool_result& result,
    const agent::tools::tool_descriptor& descriptor,
    const agent::tools::json_schema_validator& validator) {
    if (!result.succeeded() ||
        descriptor.output_validation == agent::tools::tool_output_validation_mode::disabled) {
      return;
    }
    const auto validation = validator.validate(result.data, descriptor.output_schema);
    if (validation)
      return;
    result.metadata["output_schema_valid"] = "false";
    result.metadata["output_schema_issues"] = schema_issues_json(validation);
    if (descriptor.output_validation == agent::tools::tool_output_validation_mode::strict) {
      result.content = "tool result does not satisfy its output schema";
      result.error_code = std::make_error_code(std::errc::protocol_error);
      result.error_category = agent::tools::tool_error_category::internal;
      result.retryable = false;
    }
  }

  static void populate_resource_version(
    llm_tool_result& result, const agent::tools::tool_descriptor& descriptor) {
    if (result.resource_version || descriptor.resource_version.outcome_json_pointer.empty())
      return;
    try {
      const auto& value = result.data.at(
        nlohmann::json::json_pointer(descriptor.resource_version.outcome_json_pointer));
      if (value.is_null() || value.is_array() || value.is_object())
        return;
      result.resource_version = value.is_string() ? value.get<std::string>() : value.dump();
    }
    catch (...) {
      // Resource version output is optional unless a tool reports it.
    }
  }

  bool compensate_failure(const agent::tools::tool_invocation& invocation, llm_tool_result& failure,
    const llm_agent_run_options& options) const {
    if (!invocation.descriptor.compensation.enabled || !compensate_)
      return false;
    auto compensation_invocation = invocation;
    compensation_invocation.context = options.context;
    compensation_invocation.context.stop_token = invocation.stop_token;
    compensation_invocation.descriptor.heartbeat = {};
    if (compensation_invocation.descriptor.compensation.timeout >
        std::chrono::milliseconds::zero()) {
      const auto deadline =
        std::chrono::system_clock::now() + compensation_invocation.descriptor.compensation.timeout;
      if (!compensation_invocation.context.deadline ||
          deadline < *compensation_invocation.context.deadline) {
        compensation_invocation.context.deadline = deadline;
      }
    }
    const tool_callable compensate = [callback = compensate_, failure](
                                       const agent::tools::tool_invocation& current) {
      return callback(current, failure);
    };
    for (std::size_t attempt = 1; attempt <= invocation.descriptor.compensation.max_attempts;
         ++attempt) {
      compensation_invocation.attempt = attempt;
      auto result = execute_tool_attempt(compensation_invocation, compensate, options, false);
      normalize_tool_outcome(result);
      if (result.succeeded()) {
        failure.metadata["compensation_attempts"] = std::to_string(attempt);
        return true;
      }
      failure.metadata["compensation_error"] = result.content;
      failure.metadata["compensation_error_category"] =
        agent::tools::to_string(result.error_category);
    }
    failure.metadata["compensation_attempts"] =
      std::to_string(invocation.descriptor.compensation.max_attempts);
    return false;
  }

  static bool retryable_by_policy(
    const llm_tool_result& result, const agent::tools::tool_retry_policy& policy) {
    return policy.retryable_categories.empty()
             ? result.retryable
             : policy.retryable_categories.contains(result.error_category);
  }

  static bool retry_is_safe(const agent::tools::tool_descriptor& descriptor, bool compensated) {
    return descriptor.idempotency == agent::tools::tool_idempotency::idempotent ||
           descriptor.idempotency == agent::tools::tool_idempotency::idempotent_with_key ||
           compensated;
  }

  static std::chrono::milliseconds retry_delay(
    const agent::tools::tool_retry_policy& policy, std::size_t completed_attempts) {
    double delay = static_cast<double>(policy.initial_backoff.count());
    for (std::size_t index = 1; index < completed_attempts; ++index) {
      delay *= policy.backoff_multiplier;
      delay = (std::min)(delay, static_cast<double>(policy.maximum_backoff.count()));
    }
    double jitter_factor = 1.0;
    try {
      thread_local std::mt19937_64 random(std::random_device {}());
      std::uniform_real_distribution<double> jitter(
        1.0 - policy.jitter_ratio, 1.0 + policy.jitter_ratio);
      jitter_factor = jitter(random);
    }
    catch (...) {
    }
    delay *= jitter_factor;
    delay = (std::max)(0.0, delay);
    const auto maximum_count = policy.maximum_backoff.count();
    if (!std::isfinite(delay) || delay >= static_cast<double>(maximum_count)) {
      return policy.maximum_backoff;
    }
    return std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(delay));
  }

  static void emit_pending_heartbeat(const agent::tools::tool_invocation& invocation,
    const llm_agent_callbacks& callbacks, const std::shared_ptr<tool_heartbeat_state>& state) {
    std::optional<agent::tools::tool_heartbeat> heartbeat;
    {
      std::scoped_lock lock(state->mutex);
      heartbeat = std::move(state->pending);
      state->pending.reset();
    }
    if (!heartbeat)
      return;
    const llm_tool_call call {
      .id = invocation.call_id,
      .name = invocation.name,
      .arguments_json = invocation.arguments_json,
    };
    emit_agent_event(callbacks,
      {
        .type = llm_agent_event_type::tool_heartbeat,
        .message = heartbeat->message,
        .tool_call = &call,
      });
    if (callbacks.on_tool_heartbeat) {
      callbacks.on_tool_heartbeat(call, *heartbeat);
    }
  }

  static void validate_run_options(const llm_agent_run_options& options) {
    if (options.max_in_flight_tool_invocations == 0) {
      throw std::invalid_argument(
        "agent run max in-flight tool invocations must be greater than zero");
    }
    if (options.max_tool_round_windows == 0) {
      throw std::invalid_argument(
        "agent run max tool-round windows must be greater than zero");
    }
    if (options.pricing && !agent::llm::valid_llm_pricing(*options.pricing)) {
      throw std::invalid_argument("agent run pricing is invalid");
    }
    if (options.tool_output_projection) {
      const auto validation = agent::llm::check_tool_output_projection_policy(
        *options.tool_output_projection, tool_output_estimator(options));
      if (!validation) {
        throw std::invalid_argument(validation.message.empty()
                                      ? "agent run tool output projection policy is invalid"
                                      : validation.message);
      }
    }
  }

  [[nodiscard]] static const agent::llm::context_token_estimator& tool_output_estimator(
    const llm_agent_run_options& options) {
    if (options.token_estimator) {
      return *options.token_estimator;
    }
    static const agent::llm::heuristic_context_token_estimator fallback;
    return fallback;
  }

  llm_tool_authorization authorize_tool(
    const agent::tools::tool_invocation& invocation, const llm_agent_run_options& options) const {
    if (options.callbacks.authorize_tool_call) {
      const auto decision = options.callbacks.authorize_tool_call(invocation);
      if (decision.kind != llm_tool_authorization_kind::allow) {
        return decision;
      }
    }
    if (invocation.descriptor.approval == agent::tools::tool_approval_mode::never) {
      return {};
    }
    if (!options.approval_service) {
      return {
        .kind = options.runtime ? llm_tool_authorization_kind::suspend
                                : llm_tool_authorization_kind::deny,
        .reason = "tool contract requires approval but no approval service is configured",
      };
    }

    auto request = agent::approval::make_approval_request(options.context,
      options.context.run_id + ":" + invocation.call_id,
      "Approve tool '" + invocation.name + "'",
      {},
      {
        { "run_id", options.context.run_id },
        { "trace_id", options.context.trace_id },
        { "tool_call_id", invocation.call_id },
        { "tool_version", invocation.descriptor.version },
        { "side_effect", agent::tools::to_string(invocation.descriptor.side_effect) },
      });
    for (const auto& requirement : invocation.descriptor.capabilities) {
      request.capabilities.push_back({
        .name = requirement.name,
        .risk = requirement.risk,
        .summary = requirement.summary,
        .resources = requirement.resources,
        .tool_name = invocation.name,
        .trace_id = options.context.trace_id,
        .subject_id = options.context.user_id,
        .metadata = requirement.metadata,
      });
    }
    const auto decision = options.approval_service->decide(request);
    switch (decision.kind) {
      case agent::approval::approval_decision_kind::approved:
        return {};
      case agent::approval::approval_decision_kind::denied:
        return {
          .kind = llm_tool_authorization_kind::deny,
          .reason = decision.reason.empty() ? "tool approval denied" : decision.reason,
          .metadata = decision.metadata,
        };
      case agent::approval::approval_decision_kind::needs_manual_review:
        return {
          .kind = llm_tool_authorization_kind::suspend,
          .reason =
            decision.reason.empty() ? "tool approval requires manual review" : decision.reason,
          .metadata = decision.metadata,
        };
    }
    return {
      .kind = llm_tool_authorization_kind::deny,
      .reason = "invalid approval decision",
    };
  }

  static std::string make_idempotency_key(
    const agent::core::agent_execution_context& context, const llm_tool_call& call) {
    return context.run_id.empty() ? call.id : context.run_id + ":" + call.id;
  }

  static void normalize_tool_outcome(llm_tool_result& result) {
    if (!result.error_code) {
      result.error_category = agent::tools::tool_error_category::none;
      return;
    }
    if (result.error_category != agent::tools::tool_error_category::none) {
      return;
    }
    if (result.error_code == std::errc::invalid_argument) {
      result.error_category = agent::tools::tool_error_category::invalid_input;
    }
    else if (result.error_code == std::errc::permission_denied) {
      result.error_category = agent::tools::tool_error_category::permission_denied;
    }
    else if (result.error_code == std::errc::timed_out) {
      result.error_category = agent::tools::tool_error_category::timeout;
      result.retryable = true;
    }
    else if (result.error_code == std::errc::operation_canceled) {
      result.error_category = agent::tools::tool_error_category::cancelled;
    }
    else {
      result.error_category = agent::tools::tool_error_category::internal;
    }
  }

  static llm_response denied_tool_response(const llm_tool_call& call, std::string reason) {
    llm_response response {
      .content = reason.empty() ? "Tool call denied." : std::move(reason),
      .error_code = agent::make_error_code(agent::llm_error_code::tool_call_denied),
      .stop_reason = "tool_call_denied",
    };
    response.metadata["tool_call_id"] = call.id;
    response.metadata["tool_name"] = call.name;
    return response;
  }

  static llm_response tool_output_projection_failure_response(
    const llm_tool_call& call, agent::llm::tool_output_projection_error error) {
    llm_response response {
      .content = "Tool output could not be projected within the configured model-visible limits.",
      .error_code = agent::make_error_code(agent::llm_error_code::tool_output_projection_failed),
      .stop_reason = "tool_output_projection_failed",
    };
    response.metadata["tool_call_id"] = call.id;
    response.metadata["tool_name"] = call.name;
    response.metadata["tool_output_projection_error"] = std::string(agent::llm::to_string(error));
    return response;
  }

  static llm_response invalid_tool_call_response(const llm_tool_call& call, std::string reason) {
    llm_response response {
      .content = reason.empty() ? "Invalid model tool call." : std::move(reason),
      .error_code = agent::make_error_code(agent::llm_error_code::invalid_response),
      .stop_reason = "invalid_tool_call",
    };
    response.metadata["tool_call_id"] = call.id;
    response.metadata["tool_name"] = call.name;
    return response;
  }

  static llm_response run_state_conflict_response(
    const std::string& run_id, std::uint64_t actual_revision) {
    llm_response response {
      .content = "Agent run state changed concurrently.",
      .error_code = agent::make_error_code(agent::llm_error_code::run_state_conflict),
      .stop_reason = "run_state_conflict",
    };
    response.metadata["run_id"] = run_id;
    response.metadata["actual_revision"] = std::to_string(actual_revision);
    return response;
  }

  static llm_response timeout_response(const llm_agent_callbacks& callbacks) {
    llm_response response {
      .content = "Agent run deadline reached.",
      .error_code = agent::make_error_code(agent::llm_error_code::timeout),
      .stop_reason = "deadline_reached",
    };
    emit_error(callbacks, response);
    return response;
  }

  bool should_stream(const llm_agent_callbacks& callbacks) const {
    return client_.supports_streaming() &&
           (static_cast<bool>(callbacks.on_delta) ||
             static_cast<bool>(callbacks.on_reasoning_delta) ||
             static_cast<bool>(callbacks.on_reasoning_done) ||
             static_cast<bool>(callbacks.on_event) || static_cast<bool>(callbacks.on_stream_event));
  }

  static void emit_nonstreaming_content(
    const llm_agent_callbacks& callbacks, const llm_response& response, bool use_streaming) {
    if (!use_streaming) {
      emit_delta(callbacks, response);
      emit_reasoning_done(callbacks, response);
    }
  }

  static void emit_delta(const llm_agent_callbacks& callbacks, const llm_response& response) {
    if (callbacks.on_delta && !response.content.empty()) {
      callbacks.on_delta(response.content);
    }
  }

  static void emit_reasoning_done(
    const llm_agent_callbacks& callbacks, const llm_response& response) {
    if (response.reasoning_summary.empty()) {
      return;
    }
    llm_stream_event stream_event {
      .type = llm_stream_event_type::reasoning_done,
      .reasoning_summary = response.reasoning_summary,
      .reasoning_metadata = response.reasoning_metadata,
      .response = response,
    };
    if (callbacks.on_reasoning_done) {
      callbacks.on_reasoning_done(response.reasoning_summary);
    }
    emit_agent_event(callbacks,
      {
        .type = llm_agent_event_type::model_reasoning_completed,
        .message = response.reasoning_summary,
        .stream_event = &stream_event,
        .response = &response,
      });
  }

  llm_response complete_model(llm_request request, const llm_agent_run_options& options,
    std::stop_token stop_token, bool use_streaming) const {
    const auto& callbacks = options.callbacks;
    const auto execution_context = request.execution_context;
    auto prepared_request = prepare_model_request(callbacks, std::move(request));
    if (!prepared_request) {
      return cancelled_response(callbacks);
    }
    request = std::move(*prepared_request);
    if (execution_context) {
      request.execution_context = execution_context;
    }
    if (request.context_budget) {
      const auto budget = *request.context_budget;
      agent::llm::context_budget_manager manager(options.token_estimator);
      auto fitted = manager.fit(std::move(request), budget);
      if (!fitted) {
        llm_response response {
          .content = "LLM request exceeds its configured context budget: " + fitted.report.error,
          .error_code = agent::make_error_code(agent::llm_error_code::context_budget_exceeded),
        };
        response.metadata["context_budget_error"] = fitted.report.error;
        response.metadata["input_tokens_before"] =
          std::to_string(fitted.report.before.input_total());
        response.metadata["reserved_output_tokens"] =
          std::to_string(fitted.report.before.reserved_output);
        return response;
      }
      if (callbacks.on_context_budget) {
        callbacks.on_context_budget(fitted.report);
      }
      request = std::move(fitted.request);
    }
    if (callbacks.on_model_start && !callbacks.on_model_start(request)) {
      return cancelled_response(callbacks);
    }
    emit_agent_event(callbacks,
      {
        .type = llm_agent_event_type::model_started,
        .request = &request,
      });
    if (!use_streaming) {
      auto response = client_.complete(request, stop_token);
      emit_agent_event(callbacks,
        {
          .type = llm_agent_event_type::model_completed,
          .request = &request,
          .response = &response,
        });
      emit_model_result(callbacks, request, response);
      return response;
    }

    llm_stream_callbacks stream_callbacks;
    bool saw_first_event = false;
    bool saw_reasoning_completed = false;
    stream_callbacks.on_event = [&](const llm_stream_event& event) {
      if (!saw_first_event) {
        saw_first_event = true;
        emit_agent_event(callbacks,
          {
            .type = llm_agent_event_type::model_first_event,
            .request = &request,
            .stream_event = &event,
          });
      }
      if (callbacks.on_stream_event) {
        callbacks.on_stream_event(event);
      }
      if (event.type == llm_stream_event_type::content_delta && callbacks.on_delta &&
          !event.content_delta.empty()) {
        callbacks.on_delta(event.content_delta);
      }
      if (event.type == llm_stream_event_type::content_delta && !event.content_delta.empty()) {
        emit_agent_event(callbacks,
          {
            .type = llm_agent_event_type::model_content_delta,
            .delta = event.content_delta,
            .request = &request,
            .stream_event = &event,
          });
      }
      else if (event.type == llm_stream_event_type::reasoning_delta &&
               !event.reasoning_delta.empty()) {
        if (callbacks.on_reasoning_delta) {
          callbacks.on_reasoning_delta(event.reasoning_delta);
        }
        emit_agent_event(callbacks,
          {
            .type = llm_agent_event_type::model_reasoning_delta,
            .delta = event.reasoning_delta,
            .request = &request,
            .stream_event = &event,
          });
      }
      else if (event.type == llm_stream_event_type::reasoning_done &&
               !event.reasoning_summary.empty()) {
        saw_reasoning_completed = true;
        if (callbacks.on_reasoning_done) {
          callbacks.on_reasoning_done(event.reasoning_summary);
        }
        emit_agent_event(callbacks,
          {
            .type = llm_agent_event_type::model_reasoning_completed,
            .message = event.reasoning_summary,
            .request = &request,
            .stream_event = &event,
            .response = event.response ? &*event.response : nullptr,
          });
      }
      else if (event.type == llm_stream_event_type::tool_call_delta) {
        emit_agent_event(callbacks,
          {
            .type = llm_agent_event_type::tool_call_building,
            .request = &request,
            .stream_event = &event,
          });
      }
      else if (event.type == llm_stream_event_type::tool_call_done && event.tool_call.has_value()) {
        emit_agent_event(callbacks,
          {
            .type = llm_agent_event_type::tool_call_ready,
            .message = event.tool_call->name,
            .request = &request,
            .stream_event = &event,
            .tool_call = &*event.tool_call,
          });
      }
    };
    auto response = client_.complete_stream(request, stream_callbacks, stop_token);
    if (!response.reasoning_summary.empty() && !saw_reasoning_completed) {
      emit_reasoning_done(callbacks, response);
    }
    emit_agent_event(callbacks,
      {
        .type = llm_agent_event_type::model_completed,
        .request = &request,
        .response = &response,
      });
    emit_model_result(callbacks, request, response);
    return response;
  }

  static void emit_tool_start(const llm_agent_callbacks& callbacks, const llm_tool_call& call) {
    emit_agent_event(callbacks,
      {
        .type = llm_agent_event_type::tool_started,
        .message = call.name,
        .tool_call = &call,
      });
    if (callbacks.on_tool_start) {
      callbacks.on_tool_start(call);
    }
  }

  static bool allow_tool_call(const llm_agent_callbacks& callbacks, const llm_tool_call& call) {
    return !callbacks.allow_tool_call || callbacks.allow_tool_call(call);
  }

  static std::optional<llm_tool_call> prepare_tool_call(
    const llm_agent_callbacks& callbacks, const llm_tool_call& call) {
    return callbacks.prepare_tool_call ? callbacks.prepare_tool_call(call)
                                       : std::optional<llm_tool_call>(call);
  }

  static std::optional<llm_tool_result> prepare_tool_result(
    const llm_agent_callbacks& callbacks, const llm_tool_call& call, llm_tool_result result) {
    return callbacks.prepare_tool_result ? callbacks.prepare_tool_result(call, std::move(result))
                                         : std::optional<llm_tool_result>(std::move(result));
  }

  static std::optional<llm_request> prepare_model_request(
    const llm_agent_callbacks& callbacks, llm_request request) {
    return callbacks.prepare_model_request ? callbacks.prepare_model_request(std::move(request))
                                           : std::optional<llm_request>(std::move(request));
  }

  static void emit_model_result(const llm_agent_callbacks& callbacks, const llm_request& request,
    const llm_response& response) {
    if (callbacks.on_model_result) {
      callbacks.on_model_result(request, response);
    }
  }

  static void emit_tool_result(const llm_agent_callbacks& callbacks, const llm_tool_call& call,
    const llm_tool_result& result) {
    emit_agent_event(callbacks,
      {
        .type = llm_agent_event_type::tool_completed,
        .message = result.content,
        .tool_call = &call,
        .tool_result = &result,
      });
    if (callbacks.on_tool_result) {
      callbacks.on_tool_result(call, result);
    }
  }

  static void emit_tool_output_projection(const llm_agent_callbacks& callbacks,
    const llm_tool_call& call, const agent::llm::tool_output_projection_report& report) {
    if (callbacks.on_tool_output_projection) {
      callbacks.on_tool_output_projection(call, report);
    }
    if (!report.truncated) {
      return;
    }
    emit_agent_event(callbacks,
      {
        .type = llm_agent_event_type::tool_output_truncated,
        .message = std::string(agent::llm::to_string(report.limiting_factor)),
        .tool_call = &call,
        .tool_output_projection = &report,
      });
  }

  static void emit_done(const llm_agent_callbacks& callbacks, const llm_response& response) {
    emit_agent_event(callbacks,
      {
        .type = llm_agent_event_type::agent_completed,
        .message = response.content,
        .response = &response,
      });
    if (callbacks.on_done) {
      callbacks.on_done(response);
    }
  }

  static void emit_error(const llm_agent_callbacks& callbacks, const llm_response& response) {
    emit_agent_event(callbacks,
      {
        .type = llm_agent_event_type::agent_failed,
        .message = response.content,
        .response = &response,
      });
    if (callbacks.on_error) {
      callbacks.on_error(response.error_code, response.content);
    }
  }

  static llm_response cancelled_response(const llm_agent_callbacks& callbacks) {
    emit_agent_event(callbacks, { .type = llm_agent_event_type::agent_cancelled });
    if (callbacks.on_cancelled) {
      callbacks.on_cancelled();
    }
    return {
      .error_code = agent::make_error_code(agent::llm_error_code::cancelled),
    };
  }

  static void emit_agent_event(const llm_agent_callbacks& callbacks, const llm_agent_event& event) {
    if (callbacks.on_event) {
      callbacks.on_event(event);
    }
  }

  static std::string last_user_content(const llm_request& request) {
    for (auto it = request.messages.rbegin(); it != request.messages.rend(); ++it) {
      if (it->role == "user") {
        return it->content;
      }
    }
    return {};
  }

  void observe_request_messages(
    const llm_request& request, const agent::core::agent_execution_context& context) const {
    if (!memory_) {
      return;
    }

    for (const auto& message : request.messages) {
      if (message.role != "system") {
        memory_->observe(
          message, agent::memory::memory_scope_from_execution_context(context, memory_->scope()));
      }
    }
  }

  void observe_assistant_response(const llm_response& response,
    const std::vector<llm_tool_call>* tool_calls,
    const agent::core::agent_execution_context& context) const {
    if (!memory_) {
      return;
    }

    memory_->observe({ .role = "assistant",
                       .content = response.content,
                       .tool_calls = tool_calls == nullptr ? response.tool_calls : *tool_calls },
      agent::memory::memory_scope_from_execution_context(context, memory_->scope()));
  }

private:
  llm_client& client_;
  std::function<std::vector<llm_tool>()> tools_;
  std::function<std::vector<agent::tools::tool_descriptor>()> descriptors_;
  std::function<llm_tool_result(const agent::tools::tool_invocation&)> invoke_;
  std::function<llm_tool_result(const agent::tools::tool_invocation&, const llm_tool_result&)>
    compensate_;
  std::function<agent::tools::tool_provider_capabilities(const std::string&)>
    provider_capabilities_;
  agent::memory::memory_context* memory_ {};
  int max_tool_rounds_;
  std::shared_ptr<timed_tool_execution_state> timed_tool_execution_state_ {
    std::make_shared<timed_tool_execution_state>()
  };
};

template<typename... Tools>
llm_agent_runner llm_client::bind_tools(int max_tool_rounds) {
  return llm_agent_runner(*this, std::make_shared<tool_provider<Tools...>>(), max_tool_rounds);
}

WUWE_NAMESPACE_END

#endif // WUWE_AGENT_LLM_AGENT_RUNNER_H
