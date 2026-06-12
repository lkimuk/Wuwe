#ifndef WUWE_AGENT_LLM_OPENROUTER_LLM_CLIENT_H
#define WUWE_AGENT_LLM_OPENROUTER_LLM_CLIENT_H

#include <memory>

#include <wuwe/agent/llm/openai_compatible_llm_client.h>

WUWE_NAMESPACE_BEGIN

class openrouter_llm_client final : public openai_compatible_llm_client {
public:
  explicit openrouter_llm_client(llm_client_config config);
  openrouter_llm_client(llm_client_config config, std::shared_ptr<http_client> http);

private:
  static llm_client_config normalize_config(llm_client_config config);
};

WUWE_NAMESPACE_END

#endif // WUWE_AGENT_LLM_OPENROUTER_LLM_CLIENT_H
