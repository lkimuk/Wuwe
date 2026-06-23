#include <wuwe/agent/execution/restricted_process_backend.hpp>

namespace wuwe::agent::execution {
namespace {

bool is_enforced(sandbox::enforcement_level level) {
  return level == sandbox::enforcement_level::enforced;
}

void add_if_not_enforced(
  std::vector<std::string>& blockers,
  sandbox::enforcement_level level,
  const char* name) {
  if (!is_enforced(level)) {
    blockers.emplace_back(name);
  }
}

} // namespace

sandbox::sandbox_enforcement_contract
restricted_process_backend_planned_contract() {
  return {
    .shell_execution = sandbox::enforcement_level::planned,
    .timeout = sandbox::enforcement_level::planned,
    .cancellation = sandbox::enforcement_level::planned,
    .stdout_limit = sandbox::enforcement_level::planned,
    .stderr_limit = sandbox::enforcement_level::planned,
    .environment_allowlist = sandbox::enforcement_level::planned,
    .working_directory = sandbox::enforcement_level::planned,
    .process_tree_cleanup = sandbox::enforcement_level::planned,
    .process_count_limit = sandbox::enforcement_level::planned,
    .cpu_time_limit = sandbox::enforcement_level::planned,
    .memory_limit = sandbox::enforcement_level::planned,
    .filesystem_read_deny = sandbox::enforcement_level::planned,
    .filesystem_write_deny = sandbox::enforcement_level::planned,
    .network_deny = sandbox::enforcement_level::planned,
  };
}

sandbox::sandbox_enforcement_contract
restricted_process_backend_configured_contract(
  const restricted_process_backend_config& config) {
#ifdef _WIN32
  const auto job_enforcement = config.use_job_object
                                 ? sandbox::enforcement_level::enforced
                                 : sandbox::enforcement_level::not_enforced;
  return {
    .shell_execution = sandbox::enforcement_level::enforced,
    .timeout = sandbox::enforcement_level::enforced,
    .cancellation = sandbox::enforcement_level::enforced,
    .stdout_limit = sandbox::enforcement_level::enforced,
    .stderr_limit = sandbox::enforcement_level::enforced,
    .environment_allowlist = sandbox::enforcement_level::enforced,
    .working_directory = sandbox::enforcement_level::enforced,
    .process_tree_cleanup = job_enforcement,
    .process_count_limit = job_enforcement,
    .cpu_time_limit = job_enforcement,
    .memory_limit = job_enforcement,
    .filesystem_read_deny = sandbox::enforcement_level::partial,
    .filesystem_write_deny = sandbox::enforcement_level::partial,
    .network_deny = config.deny_network
                      ? sandbox::enforcement_level::enforced
                      : sandbox::enforcement_level::not_enforced,
  };
#else
  (void)config;
  auto contract = restricted_process_backend_planned_contract();
  contract.process_tree_cleanup = sandbox::enforcement_level::not_enforced;
  contract.process_count_limit = sandbox::enforcement_level::not_enforced;
  contract.cpu_time_limit = sandbox::enforcement_level::not_enforced;
  contract.memory_limit = sandbox::enforcement_level::not_enforced;
  contract.filesystem_read_deny = sandbox::enforcement_level::not_enforced;
  contract.filesystem_write_deny = sandbox::enforcement_level::not_enforced;
  contract.network_deny = sandbox::enforcement_level::not_enforced;
  return contract;
#endif
}

restricted_process_backend_availability
evaluate_restricted_process_backend_availability(
  const restricted_process_backend_config& config) {
  restricted_process_backend_availability result {
    .contract = restricted_process_backend_configured_contract(config),
  };

#ifndef _WIN32
  result.blockers.emplace_back("restricted_process_unsupported_platform");
  return result;
#else
  add_if_not_enforced(
    result.blockers,
    result.contract.shell_execution,
    "shell_execution_not_enforced");
  add_if_not_enforced(
    result.blockers,
    result.contract.timeout,
    "timeout_not_enforced");
  add_if_not_enforced(
    result.blockers,
    result.contract.cancellation,
    "cancellation_not_enforced");
  add_if_not_enforced(
    result.blockers,
    result.contract.stdout_limit,
    "stdout_limit_not_enforced");
  add_if_not_enforced(
    result.blockers,
    result.contract.stderr_limit,
    "stderr_limit_not_enforced");
  add_if_not_enforced(
    result.blockers,
    result.contract.environment_allowlist,
    "environment_allowlist_not_enforced");
  add_if_not_enforced(
    result.blockers,
    result.contract.working_directory,
    "working_directory_not_enforced");
  add_if_not_enforced(
    result.blockers,
    result.contract.process_tree_cleanup,
    "process_tree_cleanup_not_enforced");
  add_if_not_enforced(
    result.blockers,
    result.contract.process_count_limit,
    "process_count_limit_not_enforced");
  add_if_not_enforced(
    result.blockers,
    result.contract.cpu_time_limit,
    "cpu_time_limit_not_enforced");
  add_if_not_enforced(
    result.blockers,
    result.contract.memory_limit,
    "memory_limit_not_enforced");
  add_if_not_enforced(
    result.blockers,
    result.contract.filesystem_read_deny,
    "filesystem_read_deny_not_enforced");
  add_if_not_enforced(
    result.blockers,
    result.contract.filesystem_write_deny,
    "filesystem_write_deny_not_enforced");
  add_if_not_enforced(
    result.blockers,
    result.contract.network_deny,
    "network_deny_not_enforced");

  result.available = result.blockers.empty();
  return result;
#endif
}

const char* to_string(restricted_process_runtime_staging staging) noexcept {
  switch (staging) {
    case restricted_process_runtime_staging::copy_minimal_python_runtime:
      return "copy_minimal_python_runtime";
  }
  return "unknown";
}

sandbox::sandbox_backend_info restricted_process_backend_descriptor() {
  return {
    .name = "restricted_process",
    .isolation = sandbox::isolation_level::restricted_process,
    .available = false,
    .unavailable_reason =
      "restricted_process backend is planned but not implemented in this build",
    .features = {
      sandbox::sandbox_feature::environment_allowlist,
      sandbox::sandbox_feature::working_directory,
      sandbox::sandbox_feature::stdout_capture,
      sandbox::sandbox_feature::stderr_capture,
      sandbox::sandbox_feature::timeout,
      sandbox::sandbox_feature::cancellation,
      sandbox::sandbox_feature::filesystem_read_restriction,
      sandbox::sandbox_feature::filesystem_write_restriction,
      sandbox::sandbox_feature::network_restriction,
    },
    .enforcement = restricted_process_backend_planned_contract(),
  };
}

} // namespace wuwe::agent::execution
