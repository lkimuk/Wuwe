#include <wuwe/agent/execution/execution_codec.hpp>

namespace wuwe::agent::execution {

nlohmann::json execution_result_to_json(const execution_result& result) {
  nlohmann::json output {
    { "termination_reason", to_string(result.termination_reason) },
    { "timed_out", result.timed_out },
    { "cancelled", result.cancelled },
    { "stdout_truncated", result.stdout_truncated },
    { "stderr_truncated", result.stderr_truncated },
    { "stdout_text", result.stdout_text },
    { "stderr_text", result.stderr_text },
    { "error_message", result.error_message },
    { "elapsed_ms", result.elapsed.count() },
    { "metadata", result.metadata },
  };
  if (result.exit_code.has_value()) {
    output["exit_code"] = *result.exit_code;
  }
  else {
    output["exit_code"] = nullptr;
  }
  return output;
}

} // namespace wuwe::agent::execution
