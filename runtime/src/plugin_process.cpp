#include <wuwe/runtime_host/plugin_process.hpp>

#include <algorithm>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <wuwe/agent/mcp/mcp_host_runtime.hpp>
#include <wuwe/version.hpp>

namespace wuwe::runtime_host {
namespace {

[[noreturn]] void fail(plugin_process_error_code code, std::string message) {
  throw plugin_process_error(code, "plugin process: " + std::move(message));
}

std::string bounded_detail(std::string_view detail) {
  constexpr std::size_t max_size = 512;
  std::string result;
  result.reserve((std::min)(detail.size(), max_size));
  for (const auto ch : detail.substr(0, max_size)) {
    const auto value = static_cast<unsigned char>(ch);
    result.push_back(value < 0x20 || value == 0x7f ? ' ' : ch);
  }
  if (detail.size() > max_size)
    result += "...";
  return result;
}

plugin_process_state state_from_mcp(agent::mcp::mcp_host_server_state state) {
  switch (state) {
    case agent::mcp::mcp_host_server_state::stopped:
      return plugin_process_state::stopped;
    case agent::mcp::mcp_host_server_state::starting:
      return plugin_process_state::starting;
    case agent::mcp::mcp_host_server_state::running:
      return plugin_process_state::running;
    case agent::mcp::mcp_host_server_state::failed:
    case agent::mcp::mcp_host_server_state::circuit_open:
      return plugin_process_state::failed;
  }
  return plugin_process_state::failed;
}

bool declares_capability(const plugin_package& package, std::string_view capability) {
  const auto& capabilities = package.manifest().capabilities;
  return std::find(capabilities.begin(), capabilities.end(), capability) != capabilities.end();
}

nlohmann::json response_result(nlohmann::json response, std::string_view operation) {
  if (!response.is_object())
    fail(plugin_process_error_code::protocol_error,
      std::string(operation) + " returned a non-object MCP response");
  if (const auto error = response.find("error"); error != response.end()) {
    auto message = std::string(operation) + " returned an MCP error";
    if (error->is_object()) {
      const auto detail = error->find("message");
      if (detail != error->end() && detail->is_string())
        message += ": " + bounded_detail(detail->get_ref<const std::string&>());
    }
    fail(plugin_process_error_code::protocol_error, std::move(message));
  }
  const auto result = response.find("result");
  if (result == response.end())
    fail(plugin_process_error_code::protocol_error,
      std::string(operation) + " omitted the MCP result");
  return std::move(*result);
}

} // namespace

struct plugin_process_supervisor::implementation {
  struct record {
    plugin_package_ptr package;
  };

  explicit implementation(plugin_process_options configured) : options(std::move(configured)) {
    switch (options.isolation) {
      case plugin_process_isolation::sandboxed_process:
      case plugin_process_isolation::trusted_process:
        break;
      default:
        fail(plugin_process_error_code::invalid_options, "unknown isolation mode");
    }
  }

  plugin_process_snapshot snapshot_for(const std::string& id, const record& value) const {
    const auto process = runtime.snapshot(id);
    return {
      .id = id,
      .version = value.package->manifest().version,
      .content_sha256 = value.package->content_sha256(),
      .state = state_from_mcp(process.state),
      .isolation = options.isolation,
      .permissions_enforced = false,
      .error = bounded_detail(process.error),
    };
  }

  std::map<std::string, std::string> environment_for(const plugin_package& package,
    const std::map<std::string, std::string>& granted_environment) const {
    std::map<std::string, std::string> result {
      { "WUWE_PLUGIN_ID", package.manifest().id },
      { "WUWE_PLUGIN_VERSION", package.manifest().version },
      { "WUWE_RUNTIME_API_VERSION", package.manifest().runtime_api_version },
    };
    for (const auto& name : package.manifest().permissions.environment) {
      const auto found = granted_environment.find(name);
      if (found == granted_environment.end())
        fail(plugin_process_error_code::environment_unavailable,
          "required environment variable is not granted: " + name);
      result.emplace(name, found->second);
    }
    return result;
  }

  record& record_for(const std::string& id) {
    const auto found = records.find(id);
    if (found == records.end())
      fail(plugin_process_error_code::not_found, "plugin is not loaded: " + id);
    return found->second;
  }

  const record& record_for(const std::string& id) const {
    const auto found = records.find(id);
    if (found == records.end())
      fail(plugin_process_error_code::not_found, "plugin is not loaded: " + id);
    return found->second;
  }

  void require_running(const std::string& id) const {
    if (runtime.snapshot(id).state != agent::mcp::mcp_host_server_state::running)
      fail(plugin_process_error_code::not_running, "plugin is not running: " + id);
  }

  plugin_process_snapshot start(
    plugin_package_ptr package, const std::map<std::string, std::string>& granted_environment) {
    if (!package)
      fail(plugin_process_error_code::invalid_options, "package must not be null");
    std::unique_lock lock(mutex);
    if (options.isolation != plugin_process_isolation::trusted_process) {
      fail(plugin_process_error_code::isolation_unavailable,
        "the built-in launcher does not yet provide filesystem and network sandbox enforcement");
    }
    plugin_package_ptr current;
    try {
      current = load_plugin_package(package->root());
    }
    catch (const plugin_package_error&) {
      fail(plugin_process_error_code::package_changed,
        "package is no longer admissible: " + package->manifest().id);
    }
    if (current->content_sha256() != package->content_sha256())
      fail(plugin_process_error_code::package_changed,
        "package content changed after admission: " + package->manifest().id);
    const auto id = package->manifest().id;
    if (records.contains(id))
      fail(plugin_process_error_code::already_loaded, "plugin is already loaded: " + id);

    auto environment = environment_for(*package, granted_environment);
    records.emplace(id,
      record {
        .package = package,
      });
    try {
      runtime.add_server({
        .id = id,
        .command = {
          .command = package->entrypoint_path().string(),
          .working_directory = package->root().string(),
          .environment = std::move(environment),
          .inherit_parent_environment = false,
          .terminate_process_tree = true,
        },
        .client_info = {
          .name = "wuwe-runtime",
          .version = std::string(framework_version),
        },
      });
      runtime.start_server(id);
      (void)response_result(runtime.ping(id), "ping");
      return snapshot_for(id, records.at(id));
    }
    catch (const plugin_process_error&) {
      (void)runtime.remove_server(id);
      records.erase(id);
      throw;
    }
    catch (const std::exception& exception) {
      (void)runtime.remove_server(id);
      records.erase(id);
      fail(plugin_process_error_code::launch_failed,
        "failed to start plugin " + id + ": " + bounded_detail(exception.what()));
    }
  }

  plugin_process_options options;
  mutable std::shared_mutex mutex;
  mutable agent::mcp::mcp_host_runtime runtime;
  std::map<std::string, record> records;
};

const char* to_string(plugin_process_isolation isolation) noexcept {
  switch (isolation) {
    case plugin_process_isolation::sandboxed_process:
      return "sandboxed_process";
    case plugin_process_isolation::trusted_process:
      return "trusted_process";
  }
  return "unknown";
}

const char* to_string(plugin_process_state state) noexcept {
  switch (state) {
    case plugin_process_state::stopped:
      return "stopped";
    case plugin_process_state::starting:
      return "starting";
    case plugin_process_state::running:
      return "running";
    case plugin_process_state::failed:
      return "failed";
  }
  return "unknown";
}

const char* to_string(plugin_process_error_code code) noexcept {
  switch (code) {
    case plugin_process_error_code::invalid_options:
      return "invalid_options";
    case plugin_process_error_code::isolation_unavailable:
      return "isolation_unavailable";
    case plugin_process_error_code::package_changed:
      return "package_changed";
    case plugin_process_error_code::environment_unavailable:
      return "environment_unavailable";
    case plugin_process_error_code::already_loaded:
      return "already_loaded";
    case plugin_process_error_code::not_found:
      return "not_found";
    case plugin_process_error_code::not_running:
      return "not_running";
    case plugin_process_error_code::capability_denied:
      return "capability_denied";
    case plugin_process_error_code::launch_failed:
      return "launch_failed";
    case plugin_process_error_code::protocol_error:
      return "protocol_error";
  }
  return "unknown";
}

plugin_process_error::plugin_process_error(plugin_process_error_code code, std::string message)
    : std::runtime_error(std::move(message)), code_(code) {
}

plugin_process_supervisor::plugin_process_supervisor(plugin_process_options options)
    : impl_(std::make_unique<implementation>(std::move(options))) {
}

plugin_process_supervisor::~plugin_process_supervisor() = default;

plugin_process_supervisor::plugin_process_supervisor(
  plugin_process_supervisor&&) noexcept = default;

plugin_process_supervisor& plugin_process_supervisor::operator=(
  plugin_process_supervisor&&) noexcept = default;

plugin_process_snapshot plugin_process_supervisor::start(plugin_package_ptr package) {
  return impl_->start(std::move(package), impl_->options.environment);
}

plugin_process_snapshot plugin_process_supervisor::start(
  plugin_package_ptr package, plugin_process_launch_options options) {
  return impl_->start(std::move(package), options.environment);
}

bool plugin_process_supervisor::stop(const std::string& id) {
  std::unique_lock lock(impl_->mutex);
  const auto found = impl_->records.find(id);
  if (found == impl_->records.end())
    return false;
  impl_->runtime.stop_server(id);
  return true;
}

bool plugin_process_supervisor::unload(const std::string& id) {
  std::unique_lock lock(impl_->mutex);
  const auto found = impl_->records.find(id);
  if (found == impl_->records.end())
    return false;
  (void)impl_->runtime.remove_server(id);
  impl_->records.erase(found);
  return true;
}

nlohmann::json plugin_process_supervisor::list_tools(const std::string& id) {
  std::shared_lock lock(impl_->mutex);
  const auto& record = impl_->record_for(id);
  if (!declares_capability(*record.package, "tools"))
    fail(plugin_process_error_code::capability_denied,
      "plugin did not declare the tools capability: " + id);
  impl_->require_running(id);
  try {
    return response_result(impl_->runtime.list_tools(id), "tools/list");
  }
  catch (const plugin_process_error&) {
    throw;
  }
  catch (const std::exception& exception) {
    fail(plugin_process_error_code::protocol_error,
      "tools/list failed for " + id + ": " + bounded_detail(exception.what()));
  }
}

nlohmann::json plugin_process_supervisor::call_tool(
  const std::string& id, std::string name, nlohmann::json arguments) {
  std::shared_lock lock(impl_->mutex);
  const auto& record = impl_->record_for(id);
  if (!declares_capability(*record.package, "tools"))
    fail(plugin_process_error_code::capability_denied,
      "plugin did not declare the tools capability: " + id);
  impl_->require_running(id);
  try {
    return response_result(
      impl_->runtime.call_tool(id, std::move(name), std::move(arguments)), "tools/call");
  }
  catch (const plugin_process_error&) {
    throw;
  }
  catch (const std::exception& exception) {
    fail(plugin_process_error_code::protocol_error,
      "tools/call failed for " + id + ": " + bounded_detail(exception.what()));
  }
}

plugin_process_snapshot plugin_process_supervisor::snapshot(const std::string& id) const {
  std::shared_lock lock(impl_->mutex);
  return impl_->snapshot_for(id, impl_->record_for(id));
}

std::vector<plugin_process_snapshot> plugin_process_supervisor::snapshots() const {
  std::shared_lock lock(impl_->mutex);
  std::vector<plugin_process_snapshot> result;
  result.reserve(impl_->records.size());
  for (const auto& [id, record] : impl_->records)
    result.push_back(impl_->snapshot_for(id, record));
  return result;
}

} // namespace wuwe::runtime_host
