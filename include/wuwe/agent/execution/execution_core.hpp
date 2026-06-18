#ifndef WUWE_AGENT_EXECUTION_EXECUTION_CORE_HPP
#define WUWE_AGENT_EXECUTION_EXECUTION_CORE_HPP

#include <chrono>
#include <filesystem>
#include <map>
#include <optional>
#include <string>

namespace wuwe::agent::execution {

enum class execution_language {
  python,
};

struct execution_limits {
  std::chrono::milliseconds timeout { 3000 };
  std::size_t max_stdout_bytes { 65536 };
  std::size_t max_stderr_bytes { 65536 };
  std::size_t max_code_bytes { 65536 };
  std::size_t max_stdin_bytes { 1048576 };
  std::size_t max_total_input_bytes { 1114112 };
};

struct execution_request {
  execution_language language { execution_language::python };
  std::string code;
  std::string stdin_text;
  std::filesystem::path workdir;
  execution_limits limits;
  std::map<std::string, std::string> env;
  bool use_shell { false };
  std::map<std::string, std::string> metadata;
};

enum class execution_termination_reason {
  exited,
  timeout,
  cancelled,
  launch_failed,
  policy_denied,
  approval_denied,
  backend_error,
};

struct execution_result {
  std::optional<int> exit_code;
  execution_termination_reason termination_reason {
    execution_termination_reason::backend_error
  };
  bool timed_out { false };
  bool cancelled { false };
  bool stdout_truncated { false };
  bool stderr_truncated { false };
  std::string stdout_text;
  std::string stderr_text;
  std::string error_message;
  std::chrono::milliseconds elapsed { 0 };
  std::map<std::string, std::string> metadata;
};

[[nodiscard]] std::string to_string(execution_language language);
[[nodiscard]] std::string to_string(execution_termination_reason reason);

} // namespace wuwe::agent::execution

#endif // WUWE_AGENT_EXECUTION_EXECUTION_CORE_HPP
