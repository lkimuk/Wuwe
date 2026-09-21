#ifndef WUWE_AGENT_LLM_ERROR_H
#define WUWE_AGENT_LLM_ERROR_H

#include <system_error>

#include <wuwe/common/wuwe_fwd.h>

WUWE_AGENT_NAMESPACE_BEGIN

enum class llm_error_code {
  none,
  missing_api_key,
  authentication_failed,
  rate_limited,
  model_unavailable,
  invalid_tool_arguments,
  cancelled,
  transport_error,
  http_error,
  api_error,
  invalid_response,
  empty_response,
  timeout,
  agent_loop_budget_exceeded,
  approval_required,
  tool_call_denied,
  run_state_conflict,
  circuit_open,
  rate_limit_wait_exceeded,
  context_budget_exceeded,
  invalid_request,
  unsupported_capability,
  tool_output_projection_failed,
  model_list_limit_exceeded,
};

[[nodiscard]] const ::std::error_category& llm_category() noexcept;

[[nodiscard]] inline std::error_code make_error_code(llm_error_code code) noexcept {
  return { static_cast<int>(code), llm_category() };
}

WUWE_AGENT_NAMESPACE_END

template<>
struct std::is_error_code_enum<wuwe::agent::llm_error_code> : std::true_type {};

#endif // WUWE_AGENT_LLM_ERROR_H
