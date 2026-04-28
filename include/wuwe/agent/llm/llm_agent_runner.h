#ifndef WUWE_AGENT_LLM_AGENT_RUNNER_H
#define WUWE_AGENT_LLM_AGENT_RUNNER_H

#include <functional>
#include <memory>
#include <string_view>
#include <utility>

#include <wuwe/agent/llm/llm_client.h>
#include <wuwe/agent/memory/memory_context.hpp>
#include <wuwe/agent/tools/tool.hpp>
#include <wuwe/common/wuwe_fwd.h>

WUWE_NAMESPACE_BEGIN

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
        invoke_([tool_provider](const std::string& name, const std::string& arguments_json) {
          return tool_provider->invoke(name, arguments_json);
        }),
        max_tool_rounds_(max_tool_rounds) {
  }

  template<typename ToolProvider>
  explicit llm_agent_runner(llm_client& client,
    std::shared_ptr<ToolProvider> tool_provider,
    agent::memory::memory_context* memory,
    int max_tool_rounds = 4)
      : client_(client),
        tools_([tool_provider] { return tool_provider->tools(); }),
        invoke_([tool_provider](const std::string& name, const std::string& arguments_json) {
          return tool_provider->invoke(name, arguments_json);
        }),
        memory_(memory),
        max_tool_rounds_(max_tool_rounds) {
  }

  llm_response complete(std::string_view prompt) const {
    llm_request request;
    request.messages.push_back({ .role = "user", .content = std::string(prompt) });
    return complete(std::move(request));
  }

  llm_response complete(llm_request request) const {
    const std::string query_text = last_user_content(request);
    observe_request_messages(request);

    if (tools_) {
      request.tools = tools_();
    }

    if (memory_) {
      request = memory_->augment(std::move(request), query_text);
    }

    llm_response response = client_.complete(request);
    if (response.error_code) {
      return response;
    }
    observe_assistant_response(response);

    for (int round = 0; round < max_tool_rounds_; ++round) {
      if (response.tool_calls.empty() || !invoke_) {
        return response;
      }

      chat_message assistant_message {
        .role = "assistant",
        .content = response.content,
        .tool_calls = response.tool_calls,
      };
      request.messages.push_back(assistant_message);

      for (const auto& call : response.tool_calls) {
        const llm_tool_result tool_result = invoke_(call.name, call.arguments_json);
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

      response = client_.complete(request);
      if (response.error_code) {
        return response;
      }
      observe_assistant_response(response);
    }

    response.error_code = std::make_error_code(std::errc::resource_unavailable_try_again);
    return response;
  }

private:
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
  std::function<llm_tool_result(const std::string&, const std::string&)> invoke_;
  agent::memory::memory_context* memory_ {};
  int max_tool_rounds_;
};

template<typename... Tools>
llm_agent_runner llm_client::bind_tools(int max_tool_rounds) {
  return llm_agent_runner(*this, std::make_shared<tool_provider<Tools...>>(), max_tool_rounds);
}

WUWE_NAMESPACE_END

#endif // WUWE_AGENT_LLM_AGENT_RUNNER_H
