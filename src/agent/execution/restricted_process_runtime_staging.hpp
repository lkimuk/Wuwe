#ifndef WUWE_AGENT_EXECUTION_RESTRICTED_PROCESS_RUNTIME_STAGING_HPP
#define WUWE_AGENT_EXECUTION_RESTRICTED_PROCESS_RUNTIME_STAGING_HPP

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace wuwe::agent::execution::detail {

enum class restricted_python_runtime_staging_status {
  ok,
  unsupported_platform,
  empty_source_python,
  empty_destination_home,
  source_python_not_found,
  source_python_not_regular_file,
  source_home_missing,
  source_lib_missing,
  create_destination_failed,
  copy_failed,
};

struct restricted_python_runtime_staging_request {
  std::filesystem::path source_python;
  std::filesystem::path destination_home;
  bool replace_existing { true };
};

struct restricted_python_runtime_staging_result {
  restricted_python_runtime_staging_status status {
    restricted_python_runtime_staging_status::ok
  };
  std::filesystem::path source_home;
  std::filesystem::path destination_home;
  std::filesystem::path python_executable;
  std::vector<std::filesystem::path> copied_files;
  std::error_code system_error;
  std::string detail;
};

[[nodiscard]] const char* to_string(
  restricted_python_runtime_staging_status status) noexcept;

[[nodiscard]] restricted_python_runtime_staging_result
stage_minimal_python_runtime_for_restricted_process(
  const restricted_python_runtime_staging_request& request);

} // namespace wuwe::agent::execution::detail

#endif // WUWE_AGENT_EXECUTION_RESTRICTED_PROCESS_RUNTIME_STAGING_HPP
