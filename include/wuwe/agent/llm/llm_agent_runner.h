#ifndef WUWE_AGENT_LLM_AGENT_RUNNER_H
#define WUWE_AGENT_LLM_AGENT_RUNNER_H

#include <functional>
#include <memory>
#include <string_view>
#include <utility>

#include <wuwe/agent/llm/llm_client.h>
#include <wuwe/agent/tools/tool.hpp>
#include <wuwe/common/wuwe_fwd.h>

WUWE_NAMESPACE_BEGIN

class llm_agent_runner {
public:
  explicit llm_agent_runner(llm_client& client, int max_tool_rounds = 4)
      : client_(client), max_tool_rounds_(max_tool_rounds) {
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

  llm_response complete(std::string_view prompt) const {
    llm_request request;
    request.messages.push_back({ .role = "user", .content = std::string(prompt) });
    return complete(std::move(request));
  }

  llm_response complete(llm_request request) const {
    if (tools_) {
      request.tools = tools_();
    }

    llm_response response = client_.complete(request);
    if (response.error_code) {
      return response;
    }

    for (int round = 0; round < max_tool_rounds_; ++round) {
      if (response.tool_calls.empty() || !invoke_) {
        return response;
      }

      request.messages.push_back(
        { .role = "assistant", .content = response.content, .tool_calls = response.tool_calls });

      for (const auto& call : response.tool_calls) {
        const llm_tool_result tool_result = invoke_(call.name, call.arguments_json);
        request.messages.push_back({ .role = "tool",
          .content =
            tool_result.content.empty() ? tool_result.error_code.message() : tool_result.content,
          .name = call.name,
          .tool_call_id = call.id });
      }

      response = client_.complete(request);
      if (response.error_code) {
        return response;
      }
    }

    response.error_code = std::make_error_code(std::errc::resource_unavailable_try_again);
    return response;
  }

private:
  llm_client& client_;
  std::function<std::vector<llm_tool>()> tools_;
  std::function<llm_tool_result(const std::string&, const std::string&)> invoke_;
  int max_tool_rounds_;
};

template<typename... Tools>
llm_agent_runner llm_client::bind_tools(int max_tool_rounds) {
  return llm_agent_runner(*this, std::make_shared<tool_provider<Tools...>>(), max_tool_rounds);
}

WUWE_NAMESPACE_END

#endif // WUWE_AGENT_LLM_AGENT_RUNNER_H
