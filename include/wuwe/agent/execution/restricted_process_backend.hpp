#ifndef WUWE_AGENT_EXECUTION_RESTRICTED_PROCESS_BACKEND_HPP
#define WUWE_AGENT_EXECUTION_RESTRICTED_PROCESS_BACKEND_HPP

#include <chrono>
#include <filesystem>
#include <map>
#include <vector>

#include <wuwe/agent/sandbox/sandbox.hpp>

namespace wuwe::agent::execution {

enum class restricted_process_runtime_staging {
  copy_minimal_python_runtime,
};

struct restricted_process_backend_config {
  std::filesystem::path python_interpreter { "python" };
  std::filesystem::path fallback_workdir;
  std::filesystem::path runtime_staging_root;
  std::vector<std::filesystem::path> readable_roots;
  std::vector<std::filesystem::path> writable_roots;
  std::map<std::string, std::string> base_environment;
  restricted_process_runtime_staging runtime_staging {
    restricted_process_runtime_staging::copy_minimal_python_runtime
  };
  bool deny_network { true };
  bool use_job_object { true };
  bool inherit_parent_environment { false };
  bool cleanup_runtime_staging { true };
  std::chrono::milliseconds python_startup_timeout { 3000 };
};

[[nodiscard]] sandbox::sandbox_enforcement_contract
restricted_process_backend_planned_contract();

[[nodiscard]] const char* to_string(
  restricted_process_runtime_staging staging) noexcept;

[[nodiscard]] sandbox::sandbox_backend_info
restricted_process_backend_descriptor();

} // namespace wuwe::agent::execution

#endif // WUWE_AGENT_EXECUTION_RESTRICTED_PROCESS_BACKEND_HPP
