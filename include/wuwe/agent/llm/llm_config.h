#ifndef WUWE_AGENT_LLM_CONFIG_H
#define WUWE_AGENT_LLM_CONFIG_H

#include <cstdlib>
#include <optional>
#include <string>

#include <gmp/macro/platform.hpp>

#include <wuwe/common/wuwe_fwd.h>

WUWE_NAMESPACE_BEGIN

struct llm_client_config {
  static std::string load_env_value(const char* name) {
#if GMP_COMPILER_MSVC
    char* api_key = nullptr;
    size_t len = 0;
    if (_dupenv_s(&api_key, &len, name) != 0 || api_key == nullptr) {
      return std::string {};
    }
    std::string value(api_key);
    free(api_key);
    return value;
#else
    const char* api_key = std::getenv(name);
    return api_key ? std::string(api_key) : std::string {};
#endif
  }

  static std::string load_api_key_from_env() {
    auto value = load_env_value("OPENAI_API_KEY");
    if (!value.empty()) {
      return value;
    }
    return load_env_value("OPENROUTER_API_KEY");
  }

  static std::string load_openrouter_api_key_from_env() {
    auto value = load_env_value("OPENROUTER_API_KEY");
    if (!value.empty()) {
      return value;
    }
    return load_env_value("OPENAI_API_KEY");
  }

  static std::string load_anthropic_api_key_from_env() {
    return load_env_value("ANTHROPIC_API_KEY");
  }

  static std::string load_gemini_api_key_from_env() {
    auto value = load_env_value("GEMINI_API_KEY");
    if (!value.empty()) {
      return value;
    }
    return load_env_value("GOOGLE_API_KEY");
  }

  static std::string load_deepseek_api_key_from_env() {
    auto value = load_env_value("DEEPSEEK_API_KEY");
    if (!value.empty()) {
      return value;
    }
    return load_env_value("OPENAI_API_KEY");
  }

  static std::string load_dashscope_api_key_from_env() {
    auto value = load_env_value("DASHSCOPE_API_KEY");
    if (!value.empty()) {
      return value;
    }
    value = load_env_value("QWEN_API_KEY");
    if (!value.empty()) {
      return value;
    }
    return load_env_value("OPENAI_API_KEY");
  }

  std::string base_url;
  std::string api_key;
  bool require_api_key { true };
  bool load_api_key_from_environment { true };
  std::string model;
  int timeout { 30000 };
  int max_retries { 2 };
  int retry_backoff_ms { 600 };
  std::optional<std::string> referer_url;
  std::optional<std::string> app_title;
};

using llm_config = llm_client_config;

WUWE_NAMESPACE_END

#endif // WUWE_AGENT_LLM_CONFIG_H
