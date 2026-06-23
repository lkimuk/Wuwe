#ifndef WUWE_AGENT_EXECUTION_RESTRICTED_PROCESS_EXECUTION_PLAN_WIN32_HPP
#define WUWE_AGENT_EXECUTION_RESTRICTED_PROCESS_EXECUTION_PLAN_WIN32_HPP

#ifdef _WIN32

#include <optional>
#include <stop_token>
#include <string>

#include <wuwe/agent/execution/execution_core.hpp>
#include <wuwe/agent/execution/restricted_process_backend.hpp>

#include "restricted_process_appcontainer_launch_win32.hpp"
#include "restricted_process_appcontainer_win32.hpp"
#include "restricted_process_request_workspace.hpp"
#include "restricted_process_runtime_staging.hpp"

namespace wuwe::agent::execution::detail {

enum class restricted_execution_plan_status {
  ok,
  unsupported_language,
  profile_failed,
  runtime_staging_failed,
  workspace_failed,
  acl_grant_failed,
};

struct restricted_execution_plan {
  restricted_appcontainer_profile profile;
  restricted_request_workspace workspace;
  restricted_python_runtime_staging_result runtime_staging;
  restricted_appcontainer_launch_request launch_request;
  std::filesystem::path runtime_root;
  std::filesystem::path python_executable;
};

struct restricted_execution_plan_result {
  restricted_execution_plan_status status {
    restricted_execution_plan_status::ok
  };
  std::optional<restricted_execution_plan> plan;
  std::string detail;
};

[[nodiscard]] const char* to_string(
  restricted_execution_plan_status status) noexcept;

[[nodiscard]] restricted_execution_plan_result prepare_restricted_execution_plan(
  const restricted_process_backend_config& config,
  const execution_request& request);

[[nodiscard]] execution_result run_restricted_execution_plan(
  const restricted_process_backend_config& config,
  const execution_request& request,
  std::stop_token stop_token = {});

} // namespace wuwe::agent::execution::detail

#endif // _WIN32

#endif // WUWE_AGENT_EXECUTION_RESTRICTED_PROCESS_EXECUTION_PLAN_WIN32_HPP
