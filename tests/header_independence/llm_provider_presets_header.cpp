#include <wuwe/agent/llm/openai_provider_presets.h>

bool llm_provider_presets_header_is_independent() {
  const wuwe::llm_client_config config { .load_api_key_from_environment = false };
  wuwe::mimo_llm_client mimo(config);
  wuwe::nvidia_llm_client nvidia(config);
  return mimo.supports_streaming() && nvidia.capabilities().tools;
}
