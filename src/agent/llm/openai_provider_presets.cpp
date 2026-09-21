#include <wuwe/agent/llm/openai_provider_presets.h>

#include <wuwe/agent/llm/llm_provider_registry.h>

#include <utility>

WUWE_NAMESPACE_BEGIN

namespace {

llm_client_config normalize_dedicated_preset(std::string_view id, llm_client_config config) {
  auto normalized = normalize_llm_client_config(id, std::move(config)).value();
  // Resolve credentials once through the preset. Do not let the generic client
  // fall back to an unrelated OPENAI_API_KEY when a dedicated key is absent.
  normalized.load_api_key_from_environment = false;
  return normalized;
}

} // namespace

openai_llm_client::openai_llm_client(llm_client_config config)
    : openai_compatible_llm_client(normalize_config(std::move(config))) {
}

openai_llm_client::openai_llm_client(llm_client_config config, std::shared_ptr<http_client> http)
    : openai_compatible_llm_client(normalize_config(std::move(config)), std::move(http)) {
}

llm_client_config openai_llm_client::normalize_config(llm_client_config config) {
  if (auto normalized = normalize_llm_client_config("OpenAI", config)) {
    return *std::move(normalized);
  }
  return config;
}

deepseek_llm_client::deepseek_llm_client(llm_client_config config)
    : openai_compatible_llm_client(normalize_config(std::move(config)), nullptr,
        {
          .normalize_dsml_tool_calls = true,
          .buffer_text_tool_protocol = true,
          .negotiate_explicit_tool_choice = true,
          .replay_reasoning_content = true,
          .request_thinking_control = true,
        }) {
}

deepseek_llm_client::deepseek_llm_client(
  llm_client_config config, std::shared_ptr<http_client> http)
    : openai_compatible_llm_client(normalize_config(std::move(config)), std::move(http),
        {
          .normalize_dsml_tool_calls = true,
          .buffer_text_tool_protocol = true,
          .negotiate_explicit_tool_choice = true,
          .replay_reasoning_content = true,
          .request_thinking_control = true,
        }) {
}

llm_client_config deepseek_llm_client::normalize_config(llm_client_config config) {
  if (auto normalized = normalize_llm_client_config("DeepSeek", config)) {
    return *std::move(normalized);
  }
  return config;
}

dashscope_llm_client::dashscope_llm_client(llm_client_config config)
    : openai_compatible_llm_client(normalize_config(std::move(config))) {
}

dashscope_llm_client::dashscope_llm_client(
  llm_client_config config, std::shared_ptr<http_client> http)
    : openai_compatible_llm_client(normalize_config(std::move(config)), std::move(http)) {
}

llm_client_config dashscope_llm_client::normalize_config(llm_client_config config) {
  if (auto normalized = normalize_llm_client_config("DashScope", config)) {
    return *std::move(normalized);
  }
  return config;
}

qwen_llm_client::qwen_llm_client(llm_client_config config)
    : openai_compatible_llm_client(normalize_config(std::move(config))) {
}

qwen_llm_client::qwen_llm_client(llm_client_config config, std::shared_ptr<http_client> http)
    : openai_compatible_llm_client(normalize_config(std::move(config)), std::move(http)) {
}

llm_client_config qwen_llm_client::normalize_config(llm_client_config config) {
  if (auto normalized = normalize_llm_client_config("Qwen", config)) {
    return *std::move(normalized);
  }
  return config;
}

zhipu_llm_client::zhipu_llm_client(llm_client_config config)
    : openai_compatible_llm_client(normalize_config(std::move(config))) {
}

zhipu_llm_client::zhipu_llm_client(llm_client_config config, std::shared_ptr<http_client> http)
    : openai_compatible_llm_client(normalize_config(std::move(config)), std::move(http)) {
}

llm_client_config zhipu_llm_client::normalize_config(llm_client_config config) {
  if (auto normalized = normalize_llm_client_config("Zhipu", config)) {
    return *std::move(normalized);
  }
  return config;
}

kimi_llm_client::kimi_llm_client(llm_client_config config)
    : kimi_llm_client(std::move(config), nullptr) {
}

kimi_llm_client::kimi_llm_client(llm_client_config config, std::shared_ptr<http_client> http)
    : openai_compatible_llm_client(normalize_dedicated_preset("Kimi", std::move(config)),
        std::move(http), { .replay_reasoning_content = true, .request_thinking_control = true }) {
}

minimax_llm_client::minimax_llm_client(llm_client_config config)
    : minimax_llm_client(std::move(config), nullptr) {
}

minimax_llm_client::minimax_llm_client(llm_client_config config, std::shared_ptr<http_client> http)
    : openai_compatible_llm_client(
        normalize_dedicated_preset("MiniMax", std::move(config)), std::move(http)) {
}

siliconflow_llm_client::siliconflow_llm_client(llm_client_config config)
    : siliconflow_llm_client(std::move(config), nullptr) {
}

siliconflow_llm_client::siliconflow_llm_client(
  llm_client_config config, std::shared_ptr<http_client> http)
    : openai_compatible_llm_client(normalize_dedicated_preset("SiliconFlow", std::move(config)),
        std::move(http), { .replay_reasoning_content = true }) {
}

doubao_llm_client::doubao_llm_client(llm_client_config config)
    : doubao_llm_client(std::move(config), nullptr) {
}

doubao_llm_client::doubao_llm_client(llm_client_config config, std::shared_ptr<http_client> http)
    : openai_compatible_llm_client(
        normalize_dedicated_preset("Doubao", std::move(config)), std::move(http)) {
}

nvidia_llm_client::nvidia_llm_client(llm_client_config config)
    : nvidia_llm_client(std::move(config), nullptr) {
}

nvidia_llm_client::nvidia_llm_client(llm_client_config config, std::shared_ptr<http_client> http)
    : openai_compatible_llm_client(normalize_dedicated_preset("Nvidia", std::move(config)),
        std::move(http), { .replay_reasoning_content = true }) {
}

stepfun_llm_client::stepfun_llm_client(llm_client_config config)
    : stepfun_llm_client(std::move(config), nullptr) {
}

stepfun_llm_client::stepfun_llm_client(llm_client_config config, std::shared_ptr<http_client> http)
    : openai_compatible_llm_client(normalize_dedicated_preset("StepFun", std::move(config)),
        std::move(http), { .replay_reasoning_content = true }) {
}

mimo_llm_client::mimo_llm_client(llm_client_config config)
    : mimo_llm_client(std::move(config), nullptr) {
}

mimo_llm_client::mimo_llm_client(llm_client_config config, std::shared_ptr<http_client> http)
    : openai_compatible_llm_client(normalize_dedicated_preset("MiMo", std::move(config)),
        std::move(http),
        { .replay_reasoning_content = true,
          .request_thinking_control = true,
          .use_max_completion_tokens = true }) {
}

WUWE_NAMESPACE_END
