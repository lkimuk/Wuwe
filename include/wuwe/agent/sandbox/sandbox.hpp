#ifndef WUWE_AGENT_SANDBOX_SANDBOX_HPP
#define WUWE_AGENT_SANDBOX_SANDBOX_HPP

#include <string>
#include <vector>

namespace wuwe::agent::sandbox {

enum class isolation_level {
  none,
  controlled_process,
  restricted_process,
  container,
  wasm,
};

enum class sandbox_feature {
  environment_allowlist,
  working_directory,
  stdout_capture,
  stderr_capture,
  timeout,
  cancellation,
  filesystem_read_restriction,
  filesystem_write_restriction,
  network_restriction,
};

enum class enforcement_level {
  not_applicable,
  not_enforced,
  partial,
  enforced,
  planned,
};

struct sandbox_enforcement_contract {
  enforcement_level shell_execution { enforcement_level::enforced };
  enforcement_level timeout { enforcement_level::not_enforced };
  enforcement_level cancellation { enforcement_level::not_enforced };
  enforcement_level stdout_limit { enforcement_level::not_enforced };
  enforcement_level stderr_limit { enforcement_level::not_enforced };
  enforcement_level environment_allowlist { enforcement_level::not_enforced };
  enforcement_level working_directory { enforcement_level::not_enforced };
  enforcement_level process_tree_cleanup { enforcement_level::not_enforced };
  enforcement_level process_count_limit { enforcement_level::not_enforced };
  enforcement_level cpu_time_limit { enforcement_level::not_enforced };
  enforcement_level memory_limit { enforcement_level::not_enforced };
  enforcement_level filesystem_read_deny { enforcement_level::not_enforced };
  enforcement_level filesystem_write_deny { enforcement_level::not_enforced };
  enforcement_level network_deny { enforcement_level::not_enforced };
};

struct sandbox_backend_info {
  std::string name;
  isolation_level isolation { isolation_level::none };
  std::vector<sandbox_feature> features;
  sandbox_enforcement_contract enforcement;
};

[[nodiscard]] inline std::string to_string(isolation_level isolation) {
  switch (isolation) {
    case isolation_level::none:
      return "none";
    case isolation_level::controlled_process:
      return "controlled_process";
    case isolation_level::restricted_process:
      return "restricted_process";
    case isolation_level::container:
      return "container";
    case isolation_level::wasm:
      return "wasm";
  }
  return "unknown";
}

[[nodiscard]] inline std::string to_string(sandbox_feature feature) {
  switch (feature) {
    case sandbox_feature::environment_allowlist:
      return "environment_allowlist";
    case sandbox_feature::working_directory:
      return "working_directory";
    case sandbox_feature::stdout_capture:
      return "stdout_capture";
    case sandbox_feature::stderr_capture:
      return "stderr_capture";
    case sandbox_feature::timeout:
      return "timeout";
    case sandbox_feature::cancellation:
      return "cancellation";
    case sandbox_feature::filesystem_read_restriction:
      return "filesystem_read_restriction";
    case sandbox_feature::filesystem_write_restriction:
      return "filesystem_write_restriction";
    case sandbox_feature::network_restriction:
      return "network_restriction";
  }
  return "unknown";
}

[[nodiscard]] inline std::string to_string(enforcement_level enforcement) {
  switch (enforcement) {
    case enforcement_level::not_applicable:
      return "not_applicable";
    case enforcement_level::not_enforced:
      return "not_enforced";
    case enforcement_level::partial:
      return "partial";
    case enforcement_level::enforced:
      return "enforced";
    case enforcement_level::planned:
      return "planned";
  }
  return "unknown";
}

} // namespace wuwe::agent::sandbox

#endif // WUWE_AGENT_SANDBOX_SANDBOX_HPP
