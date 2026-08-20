#ifndef WUWE_RUNTIME_HOST_PLUGIN_PROCESS_HPP
#define WUWE_RUNTIME_HOST_PLUGIN_PROCESS_HPP

#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <wuwe/runtime_host/plugin_package.hpp>

namespace wuwe::runtime_host {

enum class plugin_process_isolation {
  sandboxed_process,
  trusted_process,
};

[[nodiscard]] const char* to_string(plugin_process_isolation isolation) noexcept;

struct plugin_process_options {
  plugin_process_isolation isolation { plugin_process_isolation::sandboxed_process };
  std::map<std::string, std::string> environment;
};

enum class plugin_process_state {
  stopped,
  starting,
  running,
  failed,
};

[[nodiscard]] const char* to_string(plugin_process_state state) noexcept;

enum class plugin_process_error_code {
  invalid_options,
  isolation_unavailable,
  package_changed,
  environment_unavailable,
  already_loaded,
  not_found,
  not_running,
  capability_denied,
  launch_failed,
  protocol_error,
};

[[nodiscard]] const char* to_string(plugin_process_error_code code) noexcept;

class plugin_process_error : public std::runtime_error {
public:
  plugin_process_error(plugin_process_error_code code, std::string message);

  [[nodiscard]] plugin_process_error_code code() const noexcept {
    return code_;
  }

private:
  plugin_process_error_code code_;
};

struct plugin_process_snapshot {
  std::string id;
  std::string version;
  std::string content_sha256;
  plugin_process_state state { plugin_process_state::stopped };
  plugin_process_isolation isolation { plugin_process_isolation::sandboxed_process };
  bool permissions_enforced { false };
  std::string error;
};

class plugin_process_supervisor final {
public:
  explicit plugin_process_supervisor(plugin_process_options options = {});
  ~plugin_process_supervisor();

  plugin_process_supervisor(const plugin_process_supervisor&) = delete;
  plugin_process_supervisor& operator=(const plugin_process_supervisor&) = delete;
  plugin_process_supervisor(plugin_process_supervisor&&) noexcept;
  plugin_process_supervisor& operator=(plugin_process_supervisor&&) noexcept;

  [[nodiscard]] plugin_process_snapshot start(plugin_package_ptr package);
  [[nodiscard]] bool stop(const std::string& id);
  [[nodiscard]] bool unload(const std::string& id);

  [[nodiscard]] nlohmann::json list_tools(const std::string& id);
  [[nodiscard]] nlohmann::json call_tool(
    const std::string& id, std::string name, nlohmann::json arguments = nlohmann::json::object());

  [[nodiscard]] plugin_process_snapshot snapshot(const std::string& id) const;
  [[nodiscard]] std::vector<plugin_process_snapshot> snapshots() const;

private:
  struct implementation;
  std::unique_ptr<implementation> impl_;
};

} // namespace wuwe::runtime_host

#endif // WUWE_RUNTIME_HOST_PLUGIN_PROCESS_HPP
