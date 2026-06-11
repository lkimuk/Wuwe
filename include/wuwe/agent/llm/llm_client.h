#ifndef WUWE_AGENT_LLM_CLIENT_H
#define WUWE_AGENT_LLM_CLIENT_H

#include <string_view>
#include <stop_token>

#include <wuwe/agent/llm/llm_error.h>
#include <wuwe/agent/llm/llm_types.h>
#include <wuwe/common/wuwe_fwd.h>

WUWE_NAMESPACE_BEGIN

class llm_agent_runner;

class llm_client {
public:
  virtual ~llm_client() = default;

  llm_response complete(std::string_view prompt) {
    llm_request request;
    request.messages.push_back({ .role = "user", .content = std::string(prompt) });
    return complete(request);
  }

  template<typename... Tools>
  llm_agent_runner bind_tools(int max_tool_rounds = 4);

  virtual llm_response complete(const llm_request& request) = 0;

  virtual bool supports_streaming() const noexcept {
    return false;
  }

  virtual llm_response complete(const llm_request& request, std::stop_token stop_token) {
    if (stop_token.stop_requested()) {
      return { .error_code = agent::make_error_code(agent::llm_error_code::cancelled) };
    }

    auto response = complete(request);
    if (stop_token.stop_requested() && !response.error_code) {
      response.error_code = agent::make_error_code(agent::llm_error_code::cancelled);
    }
    return response;
  }

  virtual llm_response complete_stream(
    const llm_request& request,
    const llm_stream_callbacks& callbacks,
    std::stop_token stop_token = {}) {
    auto response = complete(request, stop_token);
    if (response.error_code) {
      if (callbacks.on_event) {
        callbacks.on_event({
          .type = llm_stream_event_type::error,
          .response = response,
          .error_code = response.error_code,
          .message = response.content,
        });
      }
      return response;
    }

    if (callbacks.on_event) {
      if (!response.content.empty()) {
        callbacks.on_event({
          .type = llm_stream_event_type::content_delta,
          .content_delta = response.content,
        });
      }
      for (const auto& call : response.tool_calls) {
        callbacks.on_event({
          .type = llm_stream_event_type::tool_call_done,
          .tool_call = call,
        });
      }
      callbacks.on_event({
        .type = llm_stream_event_type::done,
        .response = response,
      });
    }

    return response;
  }
};

WUWE_NAMESPACE_END

#endif // WUWE_AGENT_LLM_CLIENT_H
