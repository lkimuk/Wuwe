#ifndef WUWE_AGENT_LLM_AGENT_RUNNER_H
#define WUWE_AGENT_LLM_AGENT_RUNNER_H

#include <string_view>
#include <memory>
#include <utility>

#include <wuwe/agent/llm/llm_client.h>
#include <wuwe/agent/llm/llm_tools.h>
#include <wuwe/common/wuwe_fwd.h>

WUWE_NAMESPACE_BEGIN

namespace detail {

inline llm_response complete_request_with_tools(
  llm_client& client, llm_request request, const std::shared_ptr<const llm_tool_provider>& tool_provider,
  int max_tool_rounds) {
  if (tool_provider != nullptr) {
    request.tools = tool_provider->tools();
  }

  llm_response response = client.complete(request);
  if (response.error_code) {
    return response;
  }

  for (int round = 0; round < max_tool_rounds; ++round) {
    if (response.tool_calls.empty() || tool_provider == nullptr) {
      return response;
    }

    request.messages.push_back({
      .role = "assistant",
      .content = response.content,
      .tool_calls = response.tool_calls
    });

    for (const auto& call : response.tool_calls) {
      const llm_tool_result tool_result = tool_provider->invoke(call.name, call.arguments_json);
      request.messages.push_back({
        .role = "tool",
        .content = tool_result.content.empty() ? tool_result.error_code.message() : tool_result.content,
        .name = call.name,
        .tool_call_id = call.id
      });
    }

    response = client.complete(request);
    if (response.error_code) {
      return response;
    }
  }

  response.error_code = std::make_error_code(std::errc::resource_unavailable_try_again);
  return response;
}

} // namespace detail

class llm_agent_runner {
public:
  explicit llm_agent_runner(llm_client& client, int max_tool_rounds = 4)
      : client_(client), max_tool_rounds_(max_tool_rounds) {}

  explicit llm_agent_runner(
    llm_client& client, std::shared_ptr<const llm_tool_provider> tool_provider, int max_tool_rounds = 4)
      : client_(client), tool_provider_(std::move(tool_provider)), max_tool_rounds_(max_tool_rounds) {}

  llm_response complete(std::string_view prompt) const {
    llm_request request;
    request.messages.push_back({ .role = "user", .content = std::string(prompt) });
    return complete(std::move(request));
  }

  llm_response complete(llm_request request) const {
    return detail::complete_request_with_tools(client_, std::move(request), tool_provider_, max_tool_rounds_);
  }

private:
  llm_client& client_;
  std::shared_ptr<const llm_tool_provider> tool_provider_;
  int max_tool_rounds_;
};

template <typename... Tools>
llm_agent_runner llm_client::build_tools(int max_tool_rounds) {
  return llm_agent_runner(
    *this, std::make_shared<llm_reflected_tool_provider<Tools...>>(), max_tool_rounds);
}

WUWE_NAMESPACE_END

#endif // WUWE_AGENT_LLM_AGENT_RUNNER_H
