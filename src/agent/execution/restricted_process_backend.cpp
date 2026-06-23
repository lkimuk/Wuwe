#include <wuwe/agent/execution/restricted_process_backend.hpp>

namespace wuwe::agent::execution {

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
                      ? sandbox::enforcement_level::partial
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
