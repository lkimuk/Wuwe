#ifndef WUWE_RUNTIME_HOST_PLUGIN_RUNTIME_HPP
#define WUWE_RUNTIME_HOST_PLUGIN_RUNTIME_HPP

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <wuwe/runtime_host/plugin_catalog.hpp>
#include <wuwe/runtime_host/plugin_process.hpp>

namespace wuwe::runtime_host {

class plugin_tool_provider;

enum class plugin_runtime_error_code {
  not_registered,
  package_changed,
};

[[nodiscard]] const char* to_string(plugin_runtime_error_code code) noexcept;

class plugin_runtime_error final : public std::runtime_error {
public:
  plugin_runtime_error(plugin_runtime_error_code code, std::string message);

  [[nodiscard]] plugin_runtime_error_code code() const noexcept {
    return code_;
  }

private:
  plugin_runtime_error_code code_;
};

struct plugin_runtime_options {
  plugin_process_options process_options;
};

class plugin_runtime final {
public:
  explicit plugin_runtime(plugin_runtime_options options = {});
  ~plugin_runtime();

  plugin_runtime(const plugin_runtime&) = delete;
  plugin_runtime& operator=(const plugin_runtime&) = delete;
  plugin_runtime(plugin_runtime&&) noexcept;
  plugin_runtime& operator=(plugin_runtime&&) noexcept;

  [[nodiscard]] plugin_registration_result register_package(plugin_package_ptr package,
    plugin_registration_policy policy = plugin_registration_policy::reject_conflict);
  [[nodiscard]] plugin_registration_result register_package(const std::filesystem::path& root,
    plugin_registration_policy policy = plugin_registration_policy::reject_conflict);

  // Explicit replacement deactivates an older package with the same ID before publishing the new
  // catalog entry. Removing a package also deactivates it first.
  [[nodiscard]] bool remove_package(const std::string& id);

  // Activation is idempotent while a plugin is running. A stopped or failed process is unloaded
  // and started again from the package currently published in the catalog.
  [[nodiscard]] plugin_process_snapshot activate(const std::string& id);
  [[nodiscard]] bool deactivate(const std::string& id);

  [[nodiscard]] nlohmann::json list_tools(const std::string& id);
  [[nodiscard]] nlohmann::json call_tool(
    const std::string& id, std::string name, nlohmann::json arguments = nlohmann::json::object());

  [[nodiscard]] plugin_catalog_snapshot catalog_snapshot() const;
  [[nodiscard]] std::vector<plugin_process_snapshot> process_snapshots() const;

private:
  friend class plugin_tool_provider;

  [[nodiscard]] nlohmann::json list_tools_for_package(
    const std::string& id, std::string_view expected_content_sha256);
  [[nodiscard]] nlohmann::json call_tool_for_package(const std::string& id,
    std::string_view expected_content_sha256, std::string name, nlohmann::json arguments);

  struct implementation;
  std::unique_ptr<implementation> impl_;
};

} // namespace wuwe::runtime_host

#endif // WUWE_RUNTIME_HOST_PLUGIN_RUNTIME_HPP
