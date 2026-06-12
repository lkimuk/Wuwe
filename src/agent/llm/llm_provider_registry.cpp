#include <wuwe/agent/llm/llm_provider_registry.h>

#include <algorithm>
#include <utility>

WUWE_NAMESPACE_BEGIN

namespace {

llm_provider_capabilities cloud_chat_capabilities() {
  return {
    .streaming = true,
    .tools = true,
    .tool_choice = true,
    .json_response_format = true,
  };
}

llm_provider_capabilities anthropic_capabilities() {
  return {
    .streaming = true,
    .tools = true,
    .tool_choice = true,
  };
}

llm_provider_capabilities gemini_capabilities() {
  return {
    .streaming = true,
    .tools = true,
    .tool_choice = true,
    .multimodal_input = true,
  };
}

llm_provider_capabilities ollama_capabilities() {
  return {
    .streaming = true,
    .tools = true,
    .json_response_format = true,
    .local_runtime = true,
  };
}

std::string load_first_env_value(const std::vector<std::string>& names) {
  for (const auto& name : names) {
    auto value = llm_client_config::load_env_value(name.c_str());
    if (!value.empty()) {
      return value;
    }
  }
  return {};
}

} // namespace

std::string_view to_string(llm_provider_protocol protocol) noexcept {
  switch (protocol) {
    case llm_provider_protocol::openai_compatible:
      return "openai_compatible";
    case llm_provider_protocol::anthropic_messages:
      return "anthropic_messages";
    case llm_provider_protocol::gemini_generate_content:
      return "gemini_generate_content";
    case llm_provider_protocol::ollama_chat:
      return "ollama_chat";
  }
  return "unknown";
}

const std::vector<llm_provider_info>& list_llm_providers() {
  static const std::vector<llm_provider_info> providers {
    {
      .id = "OpenAI",
      .display_name = "OpenAI",
      .protocol = llm_provider_protocol::openai_compatible,
      .default_base_url = "https://api.openai.com",
      .api_key_env_names = {"OPENAI_API_KEY"},
      .capabilities = cloud_chat_capabilities(),
    },
    {
      .id = "OpenAICompatible",
      .display_name = "OpenAI-Compatible",
      .protocol = llm_provider_protocol::openai_compatible,
      .base_url_required = true,
      .api_key_env_names = {"OPENAI_API_KEY"},
      .capabilities = cloud_chat_capabilities(),
    },
    {
      .id = "OpenRouter",
      .display_name = "OpenRouter",
      .protocol = llm_provider_protocol::openai_compatible,
      .default_base_url = "https://openrouter.ai/api",
      .api_key_env_names = {"OPENROUTER_API_KEY", "OPENAI_API_KEY"},
      .capabilities = cloud_chat_capabilities(),
    },
    {
      .id = "Anthropic",
      .display_name = "Anthropic Claude",
      .protocol = llm_provider_protocol::anthropic_messages,
      .default_base_url = "https://api.anthropic.com",
      .api_key_env_names = {"ANTHROPIC_API_KEY"},
      .capabilities = anthropic_capabilities(),
    },
    {
      .id = "Gemini",
      .display_name = "Google Gemini",
      .protocol = llm_provider_protocol::gemini_generate_content,
      .default_base_url = "https://generativelanguage.googleapis.com",
      .api_key_env_names = {"GEMINI_API_KEY", "GOOGLE_API_KEY"},
      .capabilities = gemini_capabilities(),
    },
    {
      .id = "Ollama",
      .display_name = "Ollama",
      .protocol = llm_provider_protocol::ollama_chat,
      .default_base_url = "http://localhost:11434",
      .api_key_required = false,
      .capabilities = ollama_capabilities(),
    },
    {
      .id = "DeepSeek",
      .display_name = "DeepSeek",
      .protocol = llm_provider_protocol::openai_compatible,
      .default_base_url = "https://api.deepseek.com",
      .api_key_env_names = {"DEEPSEEK_API_KEY", "OPENAI_API_KEY"},
      .capabilities = cloud_chat_capabilities(),
    },
    {
      .id = "DashScope",
      .display_name = "DashScope",
      .protocol = llm_provider_protocol::openai_compatible,
      .default_base_url = "https://dashscope.aliyuncs.com/compatible-mode",
      .api_key_env_names = {"DASHSCOPE_API_KEY", "QWEN_API_KEY", "OPENAI_API_KEY"},
      .capabilities = cloud_chat_capabilities(),
    },
    {
      .id = "Qwen",
      .display_name = "Qwen",
      .protocol = llm_provider_protocol::openai_compatible,
      .default_base_url = "https://dashscope.aliyuncs.com/compatible-mode",
      .api_key_env_names = {"QWEN_API_KEY", "DASHSCOPE_API_KEY", "OPENAI_API_KEY"},
      .capabilities = cloud_chat_capabilities(),
    },
  };
  return providers;
}

const llm_provider_info* find_llm_provider(std::string_view id) noexcept {
  const auto& providers = list_llm_providers();
  const auto it = std::find_if(providers.begin(), providers.end(), [&](const auto& provider) {
    return provider.id == id;
  });
  return it == providers.end() ? nullptr : &*it;
}

std::optional<llm_client_config> make_default_llm_config(std::string_view provider_id) {
  const auto* provider = find_llm_provider(provider_id);
  if (!provider) {
    return std::nullopt;
  }
  return make_default_llm_config(*provider);
}

llm_client_config make_default_llm_config(const llm_provider_info& provider) {
  llm_client_config config;
  config.base_url = provider.default_base_url;
  config.require_api_key = provider.api_key_required;
  if (config.load_api_key_from_environment) {
    config.api_key = load_first_env_value(provider.api_key_env_names);
  }
  return config;
}

std::optional<llm_client_config> normalize_llm_client_config(
  std::string_view provider_id,
  llm_client_config config) {
  const auto* provider = find_llm_provider(provider_id);
  if (!provider) {
    return std::nullopt;
  }
  return normalize_llm_client_config(*provider, std::move(config));
}

llm_client_config normalize_llm_client_config(
  const llm_provider_info& provider,
  llm_client_config config) {
  if (config.base_url.empty()) {
    config.base_url = provider.default_base_url;
  }
  config.require_api_key = provider.api_key_required && config.require_api_key;
  if (config.api_key.empty() && config.load_api_key_from_environment) {
    config.api_key = load_first_env_value(provider.api_key_env_names);
  }
  return config;
}

WUWE_NAMESPACE_END
