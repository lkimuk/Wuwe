#ifndef WUWE_AGENT_LLM_OPENAI_LLM_CLIENT_H
#define WUWE_AGENT_LLM_OPENAI_LLM_CLIENT_H

#include <memory>

#include <nlohmann/json.hpp>

#include <wuwe/agent/llm/llm_client.h>
#include <wuwe/agent/llm/llm_config.h>
#include <wuwe/common/wuwe_fwd.h>
#include <wuwe/net/http_client.h>

WUWE_AGENT_NAMESPACE_BEGIN

using json = nlohmann::json;

class http_client;

class openai_llm_client final : public llm_client {
public:
  explicit openai_llm_client(llm_client_config config);

  llm_response complete(const llm_request &request) override;

private:
  json build_openai_payload(const llm_request &request) const;
  llm_response parse_openai_response(const http_response &response) const;

private:
  llm_client_config config_;
  std::shared_ptr<http_client> http_;
};

WUWE_AGENT_NAMESPACE_END

#endif // WUWE_AGENT_LLM_OPENAI_LLM_CLIENT_H
