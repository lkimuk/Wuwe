#include <wuwe/agent/llm/llm_provider_factory.h>

#include <wuwe/agent/llm/anthropic_llm_client.h>
#include <wuwe/agent/llm/gemini_llm_client.h>
#include <wuwe/agent/llm/ollama_llm_client.h>
#include <wuwe/agent/llm/openai_compatible_llm_client.h>
#include <wuwe/agent/llm/openai_provider_presets.h>
#include <wuwe/agent/llm/openrouter_llm_client.h>

WUWE_NAMESPACE_BEGIN

void register_builtin_llm_clients() {
  static const bool registered = [] {
    GMP_FACTORY_REGISTER(llm_client, llm_config,
      ("OpenAI", openai_llm_client),
      ("OpenAICompatible", openai_compatible_llm_client),
      ("OpenRouter", openrouter_llm_client),
      ("Anthropic", anthropic_llm_client),
      ("Gemini", gemini_llm_client),
      ("Ollama", ollama_llm_client),
      ("DeepSeek", deepseek_llm_client),
      ("DashScope", dashscope_llm_client),
      ("Qwen", qwen_llm_client),
      ("Zhipu", zhipu_llm_client),
      ("Kimi", kimi_llm_client),
      ("MiniMax", minimax_llm_client),
      ("SiliconFlow", siliconflow_llm_client),
      ("Doubao", doubao_llm_client),
      ("Nvidia", nvidia_llm_client),
      ("StepFun", stepfun_llm_client),
      ("MiMo", mimo_llm_client))
    return true;
  }();
  (void)registered;
}

llm_client_factory::llm_client_factory() {
  register_builtin_llm_clients();
}

llm_client_factory& llm_client_factory::instance() {
  static llm_client_factory factory;
  return factory;
}

llm_client* llm_client_factory::create(
  std::string_view provider_id, const llm_config& config) const {
  return llm_client_factory_base::instance().create(std::string(provider_id), config);
}

std::shared_ptr<llm_client> llm_client_factory::create_shared(
  std::string_view provider_id, const llm_config& config) const {
  return llm_client_factory_base::instance().create_shared(std::string(provider_id), config);
}

std::unique_ptr<llm_client> llm_client_factory::create_unique(
  std::string_view provider_id, const llm_config& config) const {
  return llm_client_factory_base::instance().create_unique(std::string(provider_id), config);
}

std::shared_ptr<llm_client> make_llm_client(
  std::string_view provider_id, llm_client_config config) {
  return llm_client_factory::instance().create_shared(provider_id, config);
}

WUWE_NAMESPACE_END
