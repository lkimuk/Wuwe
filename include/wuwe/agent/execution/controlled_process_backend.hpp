#ifndef WUWE_AGENT_EXECUTION_CONTROLLED_PROCESS_BACKEND_HPP
#define WUWE_AGENT_EXECUTION_CONTROLLED_PROCESS_BACKEND_HPP

#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <system_error>

#include <wuwe/agent/execution/execution_backend.hpp>

namespace wuwe::agent::execution {

struct python_interpreter_probe_request {
  std::filesystem::path interpreter;
  std::filesystem::path workdir;
  std::map<std::string, std::string> env;
  std::chrono::milliseconds timeout { 3000 };
};

enum class python_interpreter_status {
  ok,
  empty_path,
  not_found,
  not_executable,
  permission_denied,
  launch_failed,
  startup_timeout,
  invalid_python,
  unsupported_version,
};

struct python_interpreter_probe_result {
  python_interpreter_status status { python_interpreter_status::launch_failed };
  std::filesystem::path resolved_path;
  std::string version;
  std::string executable;
  std::string stdout_text;
  std::string stderr_text;
  std::error_code system_error;
  std::map<std::string, std::string> metadata;
};

struct controlled_process_backend_config {
  std::filesystem::path python_interpreter { "python" };
  std::filesystem::path fallback_workdir;
  bool use_job_object { true };
  bool validate_python_on_start { false };
  std::chrono::milliseconds python_startup_timeout { 3000 };
};

[[nodiscard]] std::string to_string(python_interpreter_status status);

[[nodiscard]] python_interpreter_probe_result probe_python_interpreter(
  const python_interpreter_probe_request& request);

class controlled_process_backend final : public execution_backend {
public:
  explicit controlled_process_backend(
    controlled_process_backend_config config = {});

  [[nodiscard]] sandbox::sandbox_backend_info info() const override;

  [[nodiscard]] execution_result run(
    const execution_request& request,
    std::stop_token stop_token) override;

private:
  [[nodiscard]] std::optional<execution_result> validate_python_interpreter(
    const execution_request& request) const;

  controlled_process_backend_config config_;
};

[[nodiscard]] std::unique_ptr<execution_backend> make_controlled_process_backend(
  controlled_process_backend_config config = {});

} // namespace wuwe::agent::execution

#endif // WUWE_AGENT_EXECUTION_CONTROLLED_PROCESS_BACKEND_HPP
