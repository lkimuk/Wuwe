#include <wuwe/agent/execution/execution_registry.hpp>

#include <utility>

#include <wuwe/agent/execution/controlled_process_backend.hpp>

namespace wuwe::agent::execution {
namespace {

bool is_enforced(sandbox::enforcement_level level) {
  return level == sandbox::enforcement_level::enforced;
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

sandbox::sandbox_backend_info planned_backend_descriptor(
  std::string name,
  sandbox::isolation_level isolation,
  sandbox::sandbox_enforcement_contract enforcement) {
  return {
    .name = std::move(name),
    .isolation = isolation,
    .available = false,
    .unavailable_reason =
      sandbox::to_string(isolation) + " backend is planned but not implemented in this build",
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
    .enforcement = std::move(enforcement),
  };
}

sandbox::sandbox_backend_info planned_wasm_descriptor() {
  auto info = planned_backend_descriptor(
    "wasm",
    sandbox::isolation_level::wasm,
    planned_wasm_contract());
  info.features = {
    sandbox::sandbox_feature::working_directory,
    sandbox::sandbox_feature::stdout_capture,
    sandbox::sandbox_feature::stderr_capture,
    sandbox::sandbox_feature::timeout,
    sandbox::sandbox_feature::cancellation,
    sandbox::sandbox_feature::filesystem_read_restriction,
    sandbox::sandbox_feature::filesystem_write_restriction,
    sandbox::sandbox_feature::network_restriction,
  };
  return info;
}

bool satisfies_requirements(
  const sandbox::sandbox_backend_info& info,
  const execution_backend_requirements& requirements) {
  if (!info.available) {
    return false;
  }
  if (requirements.isolation.has_value() &&
      info.isolation != *requirements.isolation) {
    return false;
  }

  const auto& enforcement = info.enforcement;
  return (!requirements.require_shell_disabled ||
           is_enforced(enforcement.shell_execution)) &&
         (!requirements.require_timeout ||
           is_enforced(enforcement.timeout)) &&
         (!requirements.require_cancellation ||
           is_enforced(enforcement.cancellation)) &&
         (!requirements.require_stdout_limit ||
           is_enforced(enforcement.stdout_limit)) &&
         (!requirements.require_stderr_limit ||
           is_enforced(enforcement.stderr_limit)) &&
         (!requirements.require_environment_allowlist ||
           is_enforced(enforcement.environment_allowlist)) &&
         (!requirements.require_process_tree_cleanup ||
           is_enforced(enforcement.process_tree_cleanup)) &&
         (!requirements.require_process_count_limit ||
           is_enforced(enforcement.process_count_limit)) &&
         (!requirements.require_cpu_time_limit ||
           is_enforced(enforcement.cpu_time_limit)) &&
         (!requirements.require_memory_limit ||
           is_enforced(enforcement.memory_limit)) &&
         (!requirements.require_filesystem_read_deny ||
           is_enforced(enforcement.filesystem_read_deny)) &&
         (!requirements.require_filesystem_write_deny ||
           is_enforced(enforcement.filesystem_write_deny)) &&
         (!requirements.require_network_deny ||
           is_enforced(enforcement.network_deny));
}

} // namespace

void execution_backend_registry::register_backend(std::string name, factory create) {
  entries_.push_back({ .name = std::move(name), .create = std::move(create) });
}

void execution_backend_registry::register_descriptor(sandbox::sandbox_backend_info info) {
  const auto name = info.name;
  entries_.push_back({ .name = name, .descriptor = std::move(info) });
}

std::unique_ptr<execution_backend> execution_backend_registry::create(
  const std::string& name) const {
  for (const auto& entry : entries_) {
    if (entry.name == name) {
      return entry.create ? entry.create() : nullptr;
    }
  }
  return nullptr;
}

std::optional<sandbox::sandbox_backend_info> execution_backend_registry::describe(
  const std::string& name) const {
  for (const auto& entry : entries_) {
    if (entry.name == name) {
      if (entry.descriptor.has_value()) {
        return entry.descriptor;
      }
      if (entry.create) {
        auto backend = entry.create();
        if (backend == nullptr) {
          return std::nullopt;
        }
        return backend->info();
      }
      return std::nullopt;
    }
  }
  return std::nullopt;
}

std::vector<sandbox::sandbox_backend_info> execution_backend_registry::backends() const {
  std::vector<sandbox::sandbox_backend_info> result;
  result.reserve(entries_.size());
  for (const auto& entry : entries_) {
    if (entry.descriptor.has_value()) {
      result.push_back(*entry.descriptor);
    }
    else if (entry.create) {
      auto backend = entry.create();
      if (backend == nullptr) {
        continue;
      }
      result.push_back(backend->info());
    }
  }
  return result;
}

std::optional<std::string> execution_backend_registry::select_backend_name(
  const execution_backend_requirements& requirements) const {
  for (const auto& entry : entries_) {
    if (const auto info = describe(entry.name)) {
      if (satisfies_requirements(*info, requirements)) {
        return entry.name;
      }
    }
  }
  return std::nullopt;
}

std::unique_ptr<execution_backend> execution_backend_registry::create_best(
  const execution_backend_requirements& requirements) const {
  const auto name = select_backend_name(requirements);
  return name.has_value() ? create(*name) : nullptr;
}

execution_backend_registry make_default_execution_backend_registry() {
  execution_backend_registry registry;
  registry.register_backend("controlled_process", [] {
    return make_controlled_process_backend();
  });
  registry.register_descriptor(planned_backend_descriptor(
    "restricted_process",
    sandbox::isolation_level::restricted_process,
    planned_restricted_contract()));
  registry.register_descriptor(planned_backend_descriptor(
    "container",
    sandbox::isolation_level::container,
    planned_restricted_contract()));
  registry.register_descriptor(planned_wasm_descriptor());
  return registry;
}

} // namespace wuwe::agent::execution
