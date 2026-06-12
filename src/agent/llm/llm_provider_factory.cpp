#include <wuwe/agent/llm/llm_provider_factory.h>

#include <gmp/dp/object_factory.hpp>

#include <wuwe/agent/llm/anthropic_llm_client.h>
#include <wuwe/agent/llm/gemini_llm_client.h>
#include <wuwe/agent/llm/ollama_llm_client.h>
#include <wuwe/agent/llm/openai_compatible_llm_client.h>
#include <wuwe/agent/llm/openai_provider_presets.h>
#include <wuwe/agent/llm/openrouter_llm_client.h>

#include <string>
#include <utility>

WUWE_NAMESPACE_BEGIN

namespace {

using raw_llm_client_factory = gmp::object_factory<llm_client, llm_config>;

template <typename Client>
void register_llm_client(const std::string& provider_id) {
  static raw_llm_client_factory::register_type<Client> registration(provider_id);
  (void)registration;
}

} // namespace

void register_builtin_llm_clients() {
  static const bool registered = [] {
    register_llm_client<openai_llm_client>("OpenAI");
    register_llm_client<openai_compatible_llm_client>("OpenAICompatible");
    register_llm_client<openrouter_llm_client>("OpenRouter");
    register_llm_client<anthropic_llm_client>("Anthropic");
    register_llm_client<gemini_llm_client>("Gemini");
    register_llm_client<ollama_llm_client>("Ollama");
    register_llm_client<deepseek_llm_client>("DeepSeek");
    register_llm_client<dashscope_llm_client>("DashScope");
    register_llm_client<qwen_llm_client>("Qwen");
    register_llm_client<zhipu_llm_client>("Zhipu");
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
  std::string_view provider_id,
  const llm_config& config) const {
  register_builtin_llm_clients();
  return raw_llm_client_factory::instance().create(std::string(provider_id), config);
}

std::shared_ptr<llm_client> llm_client_factory::create_shared(
  std::string_view provider_id,
  const llm_config& config) const {
  return std::shared_ptr<llm_client>(create(provider_id, config));
}

std::unique_ptr<llm_client> llm_client_factory::create_unique(
  std::string_view provider_id,
  const llm_config& config) const {
  return std::unique_ptr<llm_client>(create(provider_id, config));
}

std::shared_ptr<llm_client> make_llm_client(
  std::string_view provider_id,
  llm_client_config config) {
  return llm_client_factory::instance().create_shared(provider_id, std::move(config));
}

WUWE_NAMESPACE_END
