#include <wuwe/agent/execution/execution_registry.hpp>

#include <utility>

#include <wuwe/agent/execution/controlled_process_backend.hpp>
#include <wuwe/agent/execution/planned_execution_backends.hpp>

namespace wuwe::agent::execution {
namespace {

bool is_enforced(sandbox::enforcement_level level) {
  return level == sandbox::enforcement_level::enforced;
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

std::unique_ptr<execution_backend> execution_backend_registry::create(
  const std::string& name) const {
  for (const auto& entry : entries_) {
    if (entry.name == name) {
      return entry.create();
    }
  }
  return nullptr;
}

std::optional<sandbox::sandbox_backend_info> execution_backend_registry::describe(
  const std::string& name) const {
  for (const auto& entry : entries_) {
    if (entry.name == name) {
      if (auto backend = entry.create()) {
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
    if (auto backend = entry.create()) {
      result.push_back(backend->info());
    }
  }
  return result;
}

std::optional<std::string> execution_backend_registry::select_backend_name(
  const execution_backend_requirements& requirements) const {
  for (const auto& entry : entries_) {
    if (auto backend = entry.create()) {
      if (satisfies_requirements(backend->info(), requirements)) {
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
  registry.register_backend("restricted_process", [] {
    return make_restricted_process_backend();
  });
  registry.register_backend("container", [] {
    return make_container_backend();
  });
  registry.register_backend("wasm", [] {
    return make_wasm_backend();
  });
  return registry;
}

} // namespace wuwe::agent::execution
