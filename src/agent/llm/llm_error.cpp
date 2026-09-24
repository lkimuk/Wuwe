#include <wuwe/agent/llm/llm_error.h>

WUWE_AGENT_NAMESPACE_BEGIN

namespace {

template<typename T>
struct constant_init {
  union {
    T obj;
  };

  constexpr constant_init() noexcept : obj() {
  }

  ~constant_init() {
  }
};

class llm_category_impl final : public std::error_category {
public:
  const char* name() const noexcept final {
    return "llm";
  }

  std::string message(int code) const final {
    switch (static_cast<llm_error_code>(code)) {
      case llm_error_code::none:
        return "No error";
      case llm_error_code::missing_api_key:
        return "Missing API key";
      case llm_error_code::authentication_failed:
        return "Authentication failed";
      case llm_error_code::rate_limited:
        return "Rate limited";
      case llm_error_code::model_unavailable:
        return "Model unavailable";
      case llm_error_code::invalid_tool_arguments:
        return "Invalid tool arguments";
      case llm_error_code::cancelled:
        return "Cancelled";
      case llm_error_code::transport_error:
        return "Transport error";
      case llm_error_code::http_error:
        return "HTTP error";
      case llm_error_code::api_error:
        return "API error";
      case llm_error_code::invalid_response:
        return "Invalid response";
      case llm_error_code::empty_response:
        return "Empty response";
      case llm_error_code::timeout:
        return "Timeout";
      case llm_error_code::agent_loop_budget_exceeded:
        return "Agent tool round budget exceeded before producing a final answer";
      case llm_error_code::approval_required:
        return "Tool approval required";
      case llm_error_code::tool_call_denied:
        return "Tool call denied";
      case llm_error_code::run_state_conflict:
        return "Agent run state conflict";
      case llm_error_code::circuit_open:
        return "LLM provider circuit is open";
      case llm_error_code::rate_limit_wait_exceeded:
        return "LLM provider rate-limit wait exceeded";
      case llm_error_code::context_budget_exceeded:
        return "LLM context budget exceeded";
      case llm_error_code::invalid_request:
        return "Invalid LLM request";
      case llm_error_code::unsupported_capability:
        return "LLM provider does not support a requested capability";
      case llm_error_code::tool_output_projection_failed:
        return "Tool output projection failed";
      case llm_error_code::model_list_limit_exceeded:
        return "Model list exceeded the configured page, model, or response limit";
      case llm_error_code::response_limit_exceeded:
        return "LLM response exceeded the configured stream or event limit";
      default:
        return "Unknown LLM error";
    }
  }
};

} // namespace

const std::error_category& llm_category() noexcept {
  static constant_init<llm_category_impl> category;
  return category.obj;
}

WUWE_AGENT_NAMESPACE_END
