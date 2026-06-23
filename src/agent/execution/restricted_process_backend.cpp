#include <wuwe/agent/execution/restricted_process_backend.hpp>

#include <utility>

#ifdef _WIN32
#include "restricted_process_execution_plan_win32.hpp"
#endif

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

std::string join_blockers(const std::vector<std::string>& blockers) {
  std::string result;
  for (const auto& blocker : blockers) {
    if (!result.empty()) {
      result += ", ";
    }
    result += blocker;
  }
  return result;
}

class restricted_process_backend final : public execution_backend {
public:
  explicit restricted_process_backend(restricted_process_backend_config config)
      : config_(std::move(config)) {
  }

  [[nodiscard]] sandbox::sandbox_backend_info info() const override {
    auto descriptor = restricted_process_backend_descriptor();
    const auto availability = evaluate_restricted_process_backend_availability(
      config_,
      restricted_process_backend_registration::registered_factory);
    descriptor.available = availability.available;
    descriptor.enforcement = availability.contract;
    descriptor.unavailable_reason =
      availability.available ? std::string {} : join_blockers(availability.blockers);
    return descriptor;
  }

  [[nodiscard]] execution_result run(
    const execution_request& request,
    std::stop_token stop_token) override {
    const auto availability = evaluate_restricted_process_backend_availability(
      config_,
      restricted_process_backend_registration::registered_factory);
    if (!availability.available) {
      execution_result result {
        .termination_reason = execution_termination_reason::backend_error,
        .error_message = "restricted_process backend is unavailable",
      };
      result.metadata["backend_name"] = "restricted_process";
      result.metadata["error_code"] = "restricted_process_unavailable";
      result.metadata["availability_blockers"] = join_blockers(availability.blockers);
      return result;
    }

#ifdef _WIN32
    return detail::run_restricted_execution_plan(config_, request, stop_token);
#else
    execution_result result {
      .termination_reason = execution_termination_reason::backend_error,
      .error_message = "restricted_process backend is Windows-only",
    };
    result.metadata["backend_name"] = "restricted_process";
    result.metadata["error_code"] = "restricted_process_unsupported_platform";
    return result;
#endif
  }

private:
  restricted_process_backend_config config_;
};

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
    .filesystem_read_deny = sandbox::enforcement_level::enforced,
    .filesystem_write_deny = sandbox::enforcement_level::enforced,
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
  return evaluate_restricted_process_backend_availability(
    config,
    restricted_process_backend_registration::descriptor_only);
}

restricted_process_backend_availability
evaluate_restricted_process_backend_availability(
  const restricted_process_backend_config& config,
  restricted_process_backend_registration registration) {
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
  if (registration !=
      restricted_process_backend_registration::registered_factory) {
    result.blockers.emplace_back("restricted_process_backend_not_registered");
  }

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

std::unique_ptr<execution_backend> make_restricted_process_backend(
  restricted_process_backend_config config) {
  const auto availability = evaluate_restricted_process_backend_availability(
    config,
    restricted_process_backend_registration::registered_factory);
  if (!availability.available) {
    return nullptr;
  }
  return std::make_unique<restricted_process_backend>(std::move(config));
}

} // namespace wuwe::agent::execution
