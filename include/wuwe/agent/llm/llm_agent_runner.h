#ifndef WUWE_AGENT_LLM_AGENT_RUNNER_H
#define WUWE_AGENT_LLM_AGENT_RUNNER_H

#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <wuwe/agent/llm/llm_error.h>
#include <wuwe/agent/llm/llm_client.h>
#include <wuwe/agent/memory/memory_context.hpp>
#include <wuwe/agent/tools/tool.hpp>
#include <wuwe/common/wuwe_fwd.h>

WUWE_NAMESPACE_BEGIN

struct llm_agent_callbacks {
  std::function<bool(const llm_request&)> on_model_start;
  std::function<bool(const llm_tool_call&)> allow_tool_call;
  std::function<void(std::string_view)> on_delta;
  std::function<void(const llm_tool_call&)> on_tool_start;
  std::function<void(const llm_tool_call&, const llm_tool_result&)> on_tool_result;
  std::function<void(const llm_response&)> on_done;
  std::function<void(std::error_code, std::string_view)> on_error;
  std::function<void()> on_cancelled;
};

struct llm_agent_run_options {
  std::stop_token stop_token;
  llm_agent_callbacks callbacks;
};

class llm_agent_run {
public:
  llm_agent_run() = default;

  llm_agent_run(std::jthread worker, std::future<llm_response> future)
      : worker_(std::move(worker)), future_(std::move(future)) {
  }

  llm_agent_run(const llm_agent_run&) = delete;
  llm_agent_run& operator=(const llm_agent_run&) = delete;
  llm_agent_run(llm_agent_run&&) noexcept = default;
  llm_agent_run& operator=(llm_agent_run&&) noexcept = default;

  bool valid() const noexcept {
    return future_.valid();
  }

  void request_stop() {
    worker_.request_stop();
  }

  void wait() const {
    future_.wait();
  }

  llm_response get() {
    return future_.get();
  }

private:
  std::jthread worker_;
  std::future<llm_response> future_;
};

class llm_agent_runner {
public:
  explicit llm_agent_runner(llm_client& client, int max_tool_rounds = 4)
      : client_(client), max_tool_rounds_(max_tool_rounds) {
  }

  explicit llm_agent_runner(
    llm_client& client,
    agent::memory::memory_context* memory,
    int max_tool_rounds = 4)
      : client_(client), memory_(memory), max_tool_rounds_(max_tool_rounds) {
  }

  template<typename ToolProvider>
  explicit llm_agent_runner(llm_client& client,
    std::shared_ptr<ToolProvider> tool_provider, int max_tool_rounds = 4)
      : client_(client),
        tools_([tool_provider] { return tool_provider->tools(); }),
        max_tool_rounds_(max_tool_rounds) {
    bind_invoke(std::move(tool_provider));
  }

  template<typename ToolProvider>
  explicit llm_agent_runner(llm_client& client,
    std::shared_ptr<ToolProvider> tool_provider,
    agent::memory::memory_context* memory,
    int max_tool_rounds = 4)
      : client_(client),
        tools_([tool_provider] { return tool_provider->tools(); }),
        memory_(memory),
        max_tool_rounds_(max_tool_rounds) {
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
    const auto is_cancelled = [stop_token] {
      return stop_token.stop_requested();
    };
    return complete_impl(std::move(request), std::move(options), stop_token, is_cancelled);
  }

  llm_agent_run run_async(llm_request request, llm_agent_run_options options = {}) const {
    auto promise = std::make_shared<std::promise<llm_response>>();
    auto future = promise->get_future();

    std::jthread worker(
      [this, request = std::move(request), options = std::move(options), promise](
        std::stop_token worker_stop_token) mutable {
        const auto external_stop_token = options.stop_token;
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

        const auto run_stop_token = run_stop_source.get_token();
        const auto is_cancelled = [run_stop_token] {
          return run_stop_token.stop_requested();
        };

        try {
          promise->set_value(
            complete_impl(std::move(request), std::move(options), run_stop_token, is_cancelled));
        }
        catch (...) {
          promise->set_exception(std::current_exception());
        }
      });

    return llm_agent_run(std::move(worker), std::move(future));
  }

  llm_agent_run run_async(std::string_view prompt, llm_agent_run_options options = {}) const {
    llm_request request;
    request.messages.push_back({ .role = "user", .content = std::string(prompt) });
    return run_async(std::move(request), std::move(options));
  }

private:
  template<typename ToolProvider>
  void bind_invoke(std::shared_ptr<ToolProvider> tool_provider) {
    invoke_ =
      [tool_provider](
        const std::string& name,
        const std::string& arguments_json,
        std::stop_token stop_token) {
        if constexpr (requires {
                        tool_provider->invoke(name, arguments_json, stop_token);
                      }) {
          return tool_provider->invoke(name, arguments_json, stop_token);
        }
        else {
          return tool_provider->invoke(name, arguments_json);
        }
      };
  }

  llm_response complete_impl(
    llm_request request,
    llm_agent_run_options options,
    std::stop_token client_stop_token,
    const std::function<bool()>& is_cancelled) const {
    if (is_cancelled()) {
      return cancelled_response(options.callbacks);
    }

    const std::string query_text = last_user_content(request);
    const llm_request request_to_observe = request;

    if (tools_) {
      request.tools = tools_();
    }

    if (memory_) {
      request = memory_->augment(std::move(request), query_text);
      observe_request_messages(request_to_observe);
    }

    const bool use_streaming =
      client_.supports_streaming() && static_cast<bool>(options.callbacks.on_delta);
    llm_response response =
      complete_model(request, options.callbacks, client_stop_token, use_streaming);
    if (is_cancelled() || response.error_code == agent::llm_error_code::cancelled) {
      return cancelled_response(options.callbacks);
    }
    if (response.error_code) {
      emit_error(options.callbacks, response);
      return response;
    }
    if (!use_streaming) {
      emit_delta(options.callbacks, response);
    }
    observe_assistant_response(response);

    int used_tool_rounds = 0;
    llm_tool_call last_tool_call;
    llm_tool_result last_tool_result;

    for (int round = 0; round < max_tool_rounds_; ++round) {
      if (is_cancelled()) {
        return cancelled_response(options.callbacks);
      }
      if (response.tool_calls.empty() || !invoke_) {
        emit_done(options.callbacks, response);
        return response;
      }
      ++used_tool_rounds;

      chat_message assistant_message {
        .role = "assistant",
        .content = response.content,
        .tool_calls = response.tool_calls,
      };
      request.messages.push_back(assistant_message);

      for (const auto& call : response.tool_calls) {
        if (is_cancelled()) {
          return cancelled_response(options.callbacks);
        }

        if (!allow_tool_call(options.callbacks, call)) {
          return cancelled_response(options.callbacks);
        }
        emit_tool_start(options.callbacks, call);
        last_tool_call = call;
        const llm_tool_result tool_result = invoke_(call.name, call.arguments_json, client_stop_token);
        last_tool_result = tool_result;
        if (is_cancelled()) {
          return cancelled_response(options.callbacks);
        }
        emit_tool_result(options.callbacks, call, tool_result);
        chat_message tool_message { .role = "tool",
          .content =
            tool_result.content.empty() ? tool_result.error_code.message() : tool_result.content,
          .name = call.name,
          .tool_call_id = call.id };
        request.messages.push_back(tool_message);
        if (memory_) {
          memory_->observe(tool_message);
        }
      }

      response = complete_model(request, options.callbacks, client_stop_token, use_streaming);
      if (is_cancelled() || response.error_code == agent::llm_error_code::cancelled) {
        return cancelled_response(options.callbacks);
      }
      if (response.error_code) {
        emit_error(options.callbacks, response);
        return response;
      }
      if (!use_streaming) {
        emit_delta(options.callbacks, response);
      }
      observe_assistant_response(response);
    }

    response.error_code = agent::make_error_code(agent::llm_error_code::agent_loop_budget_exceeded);
    response.stop_reason = "tool_round_budget_exceeded";
    response.metadata["stop_reason"] = response.stop_reason;
    response.metadata["used_tool_rounds"] = std::to_string(used_tool_rounds);
    response.metadata["max_tool_rounds"] = std::to_string(max_tool_rounds_);
    response.metadata["last_tool_call"] = last_tool_call.name;
    response.metadata["last_tool_call_id"] = last_tool_call.id;
    response.metadata["last_tool_arguments"] = last_tool_call.arguments_json;
    response.metadata["last_tool_result"] = last_tool_result.content;
    response.metadata["last_model_response"] = response.content;
    response.content = "Agent tool round budget exceeded before producing a final answer.";
    emit_error(options.callbacks, response);
    return response;
  }

  static void emit_delta(const llm_agent_callbacks& callbacks, const llm_response& response) {
    if (callbacks.on_delta && !response.content.empty()) {
      callbacks.on_delta(response.content);
    }
  }

  llm_response complete_model(
    const llm_request& request,
    const llm_agent_callbacks& callbacks,
    std::stop_token stop_token,
    bool use_streaming) const {
    if (callbacks.on_model_start && !callbacks.on_model_start(request)) {
      return cancelled_response(callbacks);
    }
    if (!use_streaming) {
      return client_.complete(request, stop_token);
    }

    llm_stream_callbacks stream_callbacks;
    stream_callbacks.on_event = [&](const llm_stream_event& event) {
      if (event.type == llm_stream_event_type::content_delta && callbacks.on_delta &&
          !event.content_delta.empty()) {
        callbacks.on_delta(event.content_delta);
      }
    };
    return client_.complete_stream(request, stream_callbacks, stop_token);
  }

  static void emit_tool_start(const llm_agent_callbacks& callbacks, const llm_tool_call& call) {
    if (callbacks.on_tool_start) {
      callbacks.on_tool_start(call);
    }
  }

  static bool allow_tool_call(const llm_agent_callbacks& callbacks, const llm_tool_call& call) {
    return !callbacks.allow_tool_call || callbacks.allow_tool_call(call);
  }

  static void emit_tool_result(
    const llm_agent_callbacks& callbacks,
    const llm_tool_call& call,
    const llm_tool_result& result) {
    if (callbacks.on_tool_result) {
      callbacks.on_tool_result(call, result);
    }
  }

  static void emit_done(const llm_agent_callbacks& callbacks, const llm_response& response) {
    if (callbacks.on_done) {
      callbacks.on_done(response);
    }
  }

  static void emit_error(const llm_agent_callbacks& callbacks, const llm_response& response) {
    if (callbacks.on_error) {
      callbacks.on_error(response.error_code, response.content);
    }
  }

  static llm_response cancelled_response(const llm_agent_callbacks& callbacks) {
    if (callbacks.on_cancelled) {
      callbacks.on_cancelled();
    }
    return {
      .error_code = agent::make_error_code(agent::llm_error_code::cancelled),
    };
  }

  static std::string last_user_content(const llm_request& request) {
    for (auto it = request.messages.rbegin(); it != request.messages.rend(); ++it) {
      if (it->role == "user") {
        return it->content;
      }
    }
    return {};
  }

  void observe_request_messages(const llm_request& request) const {
    if (!memory_) {
      return;
    }

    for (const auto& message : request.messages) {
      if (message.role != "system") {
        memory_->observe(message);
      }
    }
  }

  void observe_assistant_response(const llm_response& response) const {
    if (!memory_) {
      return;
    }

    memory_->observe({ .role = "assistant",
      .content = response.content,
      .tool_calls = response.tool_calls });
  }

private:
  llm_client& client_;
  std::function<std::vector<llm_tool>()> tools_;
  std::function<llm_tool_result(const std::string&, const std::string&, std::stop_token)> invoke_;
  agent::memory::memory_context* memory_ {};
  int max_tool_rounds_;
};

template<typename... Tools>
llm_agent_runner llm_client::bind_tools(int max_tool_rounds) {
  return llm_agent_runner(*this, std::make_shared<tool_provider<Tools...>>(), max_tool_rounds);
}

WUWE_NAMESPACE_END

#endif // WUWE_AGENT_LLM_AGENT_RUNNER_H
