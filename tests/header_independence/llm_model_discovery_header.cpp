#include <wuwe/agent/llm/llm_model_discovery.h>

bool llm_model_discovery_header_is_independent() {
  std::stop_source stop;
  stop.request_stop();
  const auto result = wuwe::list_llm_models("OpenAI", {}, {}, stop.get_token());
  return result.error_code == wuwe::agent::llm_error_code::cancelled && result.models.empty();
}
