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
};

WUWE_NAMESPACE_END

#endif // WUWE_AGENT_LLM_CLIENT_H
