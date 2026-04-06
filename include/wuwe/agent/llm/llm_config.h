#ifndef WUWE_AGENT_LLM_CONFIG_H
#define WUWE_AGENT_LLM_CONFIG_H

#include <cstdlib>
#include <string>

#include <gmp/macro/platform.hpp>

#include <wuwe/common/wuwe_fwd.h>

WUWE_NAMESPACE_BEGIN

struct llm_client_config {
  static std::string load_api_key_from_env() {
#if GMP_COMPILER_MSVC
    char* api_key = nullptr;
    size_t len = 0;
    if (_dupenv_s(&api_key, &len, "OPENROUTER_API_KEY") != 0 || api_key == nullptr) {
      return std::string {};
    }
    std::string value(api_key);
    free(api_key);
    return value;
#else
    const char* api_key = std::getenv("OPENROUTER_API_KEY");
    return api_key ? std::string(api_key) : std::string {};
#endif
  }

  std::string base_url;
  std::string api_key { load_api_key_from_env() };
  std::string model;
  int timeout { 30000 };
  int max_retries { 2 };
  int retry_backoff_ms { 600 };
};

using llm_config = llm_client_config;

WUWE_NAMESPACE_END

#endif // WUWE_AGENT_LLM_CONFIG_H
