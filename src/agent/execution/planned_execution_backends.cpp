#include <wuwe/agent/execution/planned_execution_backends.hpp>

#include <utility>

namespace wuwe::agent::execution {
namespace {

std::string reason_or_default(
  const planned_backend_config& config,
  const std::string& backend_name) {
  if (!config.unavailable_reason.empty()) {
    return config.unavailable_reason;
  }
  return backend_name + " backend is planned but not implemented in this build";
}

sandbox::sandbox_enforcement_contract planned_restricted_contract() {
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

sandbox::sandbox_enforcement_contract planned_container_contract() {
  auto contract = planned_restricted_contract();
  contract.process_count_limit = sandbox::enforcement_level::planned;
  return contract;
}

sandbox::sandbox_enforcement_contract planned_wasm_contract() {
  return {
    .shell_execution = sandbox::enforcement_level::not_applicable,
    .timeout = sandbox::enforcement_level::planned,
    .cancellation = sandbox::enforcement_level::planned,
    .stdout_limit = sandbox::enforcement_level::planned,
    .stderr_limit = sandbox::enforcement_level::planned,
    .environment_allowlist = sandbox::enforcement_level::not_applicable,
    .working_directory = sandbox::enforcement_level::planned,
    .process_tree_cleanup = sandbox::enforcement_level::not_applicable,
    .process_count_limit = sandbox::enforcement_level::not_applicable,
    .cpu_time_limit = sandbox::enforcement_level::planned,
    .memory_limit = sandbox::enforcement_level::planned,
    .filesystem_read_deny = sandbox::enforcement_level::planned,
    .filesystem_write_deny = sandbox::enforcement_level::planned,
    .network_deny = sandbox::enforcement_level::planned,
  };
}

execution_result unavailable_result(
  const sandbox::sandbox_backend_info& info) {
  execution_result result {
    .termination_reason = execution_termination_reason::backend_error,
    .error_message = info.unavailable_reason,
  };
  result.metadata["backend"] = info.name;
  result.metadata["isolation"] = sandbox::to_string(info.isolation);
  result.metadata["backend_available"] = "false";
  result.metadata["backend_unavailable_reason"] = info.unavailable_reason;
  return result;
}

} // namespace

restricted_process_backend::restricted_process_backend(planned_backend_config config)
    : config_(std::move(config)) {
}

sandbox::sandbox_backend_info restricted_process_backend::info() const {
  return {
    .name = "restricted_process",
    .isolation = sandbox::isolation_level::restricted_process,
    .available = false,
    .unavailable_reason = reason_or_default(config_, "restricted_process"),
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
    .enforcement = planned_restricted_contract(),
  };
}

execution_result restricted_process_backend::run(
  const execution_request&,
  std::stop_token) {
  return unavailable_result(info());
}

container_backend::container_backend(planned_backend_config config)
    : config_(std::move(config)) {
}

sandbox::sandbox_backend_info container_backend::info() const {
  return {
    .name = "container",
    .isolation = sandbox::isolation_level::container,
    .available = false,
    .unavailable_reason = reason_or_default(config_, "container"),
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
    .enforcement = planned_container_contract(),
  };
}

execution_result container_backend::run(
  const execution_request&,
  std::stop_token) {
  return unavailable_result(info());
}

wasm_backend::wasm_backend(planned_backend_config config)
    : config_(std::move(config)) {
}

sandbox::sandbox_backend_info wasm_backend::info() const {
  return {
    .name = "wasm",
    .isolation = sandbox::isolation_level::wasm,
    .available = false,
    .unavailable_reason = reason_or_default(config_, "wasm"),
    .features = {
      sandbox::sandbox_feature::working_directory,
      sandbox::sandbox_feature::stdout_capture,
      sandbox::sandbox_feature::stderr_capture,
      sandbox::sandbox_feature::timeout,
      sandbox::sandbox_feature::cancellation,
      sandbox::sandbox_feature::filesystem_read_restriction,
      sandbox::sandbox_feature::filesystem_write_restriction,
      sandbox::sandbox_feature::network_restriction,
    },
    .enforcement = planned_wasm_contract(),
  };
}

execution_result wasm_backend::run(
  const execution_request&,
  std::stop_token) {
  return unavailable_result(info());
}

std::unique_ptr<execution_backend> make_restricted_process_backend(
  planned_backend_config config) {
  return std::make_unique<restricted_process_backend>(std::move(config));
}

std::unique_ptr<execution_backend> make_container_backend(
  planned_backend_config config) {
  return std::make_unique<container_backend>(std::move(config));
}

std::unique_ptr<execution_backend> make_wasm_backend(
  planned_backend_config config) {
  return std::make_unique<wasm_backend>(std::move(config));
}

} // namespace wuwe::agent::execution
