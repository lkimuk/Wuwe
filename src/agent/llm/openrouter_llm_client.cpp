#include <wuwe/agent/llm/openrouter_llm_client.h>

#include <wuwe/agent/llm/llm_provider_registry.h>

#include <utility>

WUWE_NAMESPACE_BEGIN

openrouter_llm_client::openrouter_llm_client(llm_client_config config)
    : openai_compatible_llm_client(normalize_config(std::move(config))) {}

openrouter_llm_client::openrouter_llm_client(
  llm_client_config config,
  std::shared_ptr<http_client> http)
    : openai_compatible_llm_client(normalize_config(std::move(config)), std::move(http)) {}

llm_client_config openrouter_llm_client::normalize_config(llm_client_config config) {
  if (auto normalized = normalize_llm_client_config("OpenRouter", config)) {
    config = std::move(*normalized);
  }
  if (!config.referer_url.has_value()) {
    config.referer_url = "https://github.com/lkimuk/gmp";
  }
  if (!config.app_title.has_value()) {
    config.app_title = "Wuwe";
  }
  return config;
}

WUWE_NAMESPACE_END
