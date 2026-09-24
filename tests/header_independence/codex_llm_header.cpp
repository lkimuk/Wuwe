#include <wuwe/agent/llm/codex_llm_client.h>

bool codex_llm_header_is_independent() {
  wuwe::codex_llm_options options;
  return options.timeout_ms > 0 && options.max_event_bytes > 0;
}
