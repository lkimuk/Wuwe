#include <wuwe/runtime_host/plugin_runtime.hpp>

#include <algorithm>
#include <mutex>
#include <shared_mutex>
#include <utility>

namespace wuwe::runtime_host {
namespace {

[[noreturn]] void fail(plugin_runtime_error_code code, std::string message) {
  throw plugin_runtime_error(code, "plugin runtime: " + std::move(message));
}

void validate_policy(plugin_registration_policy policy) {
  switch (policy) {
    case plugin_registration_policy::reject_conflict:
    case plugin_registration_policy::replace:
      return;
  }
  throw std::invalid_argument("unknown plugin registration policy");
}

} // namespace

struct plugin_runtime::implementation {
  explicit implementation(plugin_runtime_options configured)
      : processes(std::move(configured.process_options)) {
  }

  mutable std::shared_mutex lifecycle_mutex;
  plugin_catalog catalog;
  plugin_process_supervisor processes;
};

const char* to_string(plugin_runtime_error_code code) noexcept {
  switch (code) {
    case plugin_runtime_error_code::not_registered:
      return "not_registered";
    case plugin_runtime_error_code::package_changed:
      return "package_changed";
  }
  return "unknown";
}

plugin_runtime_error::plugin_runtime_error(plugin_runtime_error_code code, std::string message)
    : std::runtime_error(std::move(message)), code_(code) {
}

plugin_runtime::plugin_runtime(plugin_runtime_options options)
    : impl_(std::make_unique<implementation>(std::move(options))) {
}

plugin_runtime::~plugin_runtime() = default;

plugin_runtime::plugin_runtime(plugin_runtime&&) noexcept = default;

plugin_runtime& plugin_runtime::operator=(plugin_runtime&&) noexcept = default;

plugin_registration_result plugin_runtime::register_package(
  plugin_package_ptr package, plugin_registration_policy policy) {
  validate_policy(policy);
  if (!package)
    throw std::invalid_argument("plugin runtime cannot register a null package");

  std::scoped_lock lock(impl_->lifecycle_mutex);
  const auto existing = impl_->catalog.snapshot().find(package->manifest().id);
  if (existing && existing->content_sha256() != package->content_sha256() &&
      policy == plugin_registration_policy::replace) {
    (void)impl_->processes.unload(package->manifest().id);
  }
  return impl_->catalog.register_package(std::move(package), policy);
}

plugin_registration_result plugin_runtime::register_package(
  const std::filesystem::path& root, plugin_registration_policy policy) {
  return register_package(load_plugin_package(root), policy);
}

bool plugin_runtime::remove_package(const std::string& id) {
  std::scoped_lock lock(impl_->lifecycle_mutex);
  const auto package = impl_->catalog.snapshot().find(id);
  if (!package)
    return false;
  (void)impl_->processes.unload(id);
  return impl_->catalog.unregister_package(id);
}

plugin_process_snapshot plugin_runtime::activate(const std::string& id) {
  std::scoped_lock lock(impl_->lifecycle_mutex);
  const auto package = impl_->catalog.snapshot().find(id);
  if (!package)
    fail(plugin_runtime_error_code::not_registered, "plugin is not registered: " + id);

  const auto snapshots = impl_->processes.snapshots();
  const auto found = std::find_if(snapshots.begin(), snapshots.end(), [&](const auto& snapshot) {
    return snapshot.id == id;
  });
  if (found != snapshots.end()) {
    if (found->state == plugin_process_state::running)
      return *found;
    (void)impl_->processes.unload(id);
  }
  return impl_->processes.start(std::move(package));
}

bool plugin_runtime::deactivate(const std::string& id) {
  std::scoped_lock lock(impl_->lifecycle_mutex);
  return impl_->processes.unload(id);
}

nlohmann::json plugin_runtime::list_tools(const std::string& id) {
  std::shared_lock lock(impl_->lifecycle_mutex);
  return impl_->processes.list_tools(id);
}

nlohmann::json plugin_runtime::call_tool(
  const std::string& id, std::string name, nlohmann::json arguments) {
  std::shared_lock lock(impl_->lifecycle_mutex);
  return impl_->processes.call_tool(id, std::move(name), std::move(arguments));
}

nlohmann::json plugin_runtime::list_tools_for_package(
  const std::string& id, std::string_view expected_content_sha256) {
  std::shared_lock lock(impl_->lifecycle_mutex);
  const auto package = impl_->catalog.snapshot().find(id);
  if (!package)
    fail(plugin_runtime_error_code::not_registered, "plugin is not registered: " + id);
  if (package->content_sha256() != expected_content_sha256) {
    fail(plugin_runtime_error_code::package_changed,
      "plugin package identity changed after tool binding: " + id);
  }
  return impl_->processes.list_tools(id);
}

nlohmann::json plugin_runtime::call_tool_for_package(const std::string& id,
  std::string_view expected_content_sha256, std::string name, nlohmann::json arguments) {
  std::shared_lock lock(impl_->lifecycle_mutex);
  const auto package = impl_->catalog.snapshot().find(id);
  if (!package)
    fail(plugin_runtime_error_code::not_registered, "plugin is not registered: " + id);
  if (package->content_sha256() != expected_content_sha256) {
    fail(plugin_runtime_error_code::package_changed,
      "plugin package identity changed after tool binding: " + id);
  }
  return impl_->processes.call_tool(id, std::move(name), std::move(arguments));
}

plugin_catalog_snapshot plugin_runtime::catalog_snapshot() const {
  return impl_->catalog.snapshot();
}

std::vector<plugin_process_snapshot> plugin_runtime::process_snapshots() const {
  return impl_->processes.snapshots();
}

} // namespace wuwe::runtime_host
