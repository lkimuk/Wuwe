#include <wuwe/agent/llm/openai_provider_presets.h>

#include <wuwe/agent/llm/llm_provider_registry.h>

#include <utility>

WUWE_NAMESPACE_BEGIN

openai_llm_client::openai_llm_client(llm_client_config config)
    : openai_compatible_llm_client(normalize_config(std::move(config))) {}

openai_llm_client::openai_llm_client(
  llm_client_config config,
  std::shared_ptr<http_client> http)
    : openai_compatible_llm_client(normalize_config(std::move(config)), std::move(http)) {}

llm_client_config openai_llm_client::normalize_config(llm_client_config config) {
  if (auto normalized = normalize_llm_client_config("OpenAI", config)) {
    return *std::move(normalized);
  }
  return config;
}

deepseek_llm_client::deepseek_llm_client(llm_client_config config)
    : openai_compatible_llm_client(normalize_config(std::move(config))) {}

deepseek_llm_client::deepseek_llm_client(
  llm_client_config config,
  std::shared_ptr<http_client> http)
    : openai_compatible_llm_client(normalize_config(std::move(config)), std::move(http)) {}

llm_client_config deepseek_llm_client::normalize_config(llm_client_config config) {
  if (auto normalized = normalize_llm_client_config("DeepSeek", config)) {
    return *std::move(normalized);
  }
  return config;
}

dashscope_llm_client::dashscope_llm_client(llm_client_config config)
    : openai_compatible_llm_client(normalize_config(std::move(config))) {}

dashscope_llm_client::dashscope_llm_client(
  llm_client_config config,
  std::shared_ptr<http_client> http)
    : openai_compatible_llm_client(normalize_config(std::move(config)), std::move(http)) {}

llm_client_config dashscope_llm_client::normalize_config(llm_client_config config) {
  if (auto normalized = normalize_llm_client_config("DashScope", config)) {
    return *std::move(normalized);
  }
  return config;
}

qwen_llm_client::qwen_llm_client(llm_client_config config)
    : openai_compatible_llm_client(normalize_config(std::move(config))) {}

qwen_llm_client::qwen_llm_client(
  llm_client_config config,
  std::shared_ptr<http_client> http)
    : openai_compatible_llm_client(normalize_config(std::move(config)), std::move(http)) {}

llm_client_config qwen_llm_client::normalize_config(llm_client_config config) {
  if (auto normalized = normalize_llm_client_config("Qwen", config)) {
    return *std::move(normalized);
  }
  return config;
}

zhipu_llm_client::zhipu_llm_client(llm_client_config config)
    : openai_compatible_llm_client(normalize_config(std::move(config))) {}

zhipu_llm_client::zhipu_llm_client(
  llm_client_config config,
  std::shared_ptr<http_client> http)
    : openai_compatible_llm_client(normalize_config(std::move(config)), std::move(http)) {}

llm_client_config zhipu_llm_client::normalize_config(llm_client_config config) {
  if (auto normalized = normalize_llm_client_config("Zhipu", config)) {
    return *std::move(normalized);
  }
  return config;
}

WUWE_NAMESPACE_END
