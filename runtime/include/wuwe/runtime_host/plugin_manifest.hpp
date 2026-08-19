#ifndef WUWE_RUNTIME_HOST_PLUGIN_MANIFEST_HPP
#define WUWE_RUNTIME_HOST_PLUGIN_MANIFEST_HPP

#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <wuwe/runtime_host/validation.hpp>

namespace wuwe::runtime_host {

inline constexpr int plugin_manifest_schema_version = 1;
inline constexpr const char* runtime_plugin_api_version = "1.0";

enum class plugin_entrypoint_kind {
  process,
};

[[nodiscard]] std::string to_string(plugin_entrypoint_kind kind);

struct plugin_permissions {
  std::vector<std::string> environment;
  std::vector<std::filesystem::path> read_paths;
  std::vector<std::filesystem::path> write_paths;
  std::vector<std::string> network_hosts;
};

struct plugin_manifest {
  int schema_version { plugin_manifest_schema_version };
  std::string id;
  std::string version;
  std::string runtime_api_version { runtime_plugin_api_version };
  plugin_entrypoint_kind entrypoint_kind { plugin_entrypoint_kind::process };
  std::filesystem::path entrypoint;
  std::vector<std::string> capabilities;
  plugin_permissions permissions;
};

[[nodiscard]] validation_report validate_plugin_manifest(const nlohmann::json& document);
[[nodiscard]] plugin_manifest plugin_manifest_from_json(const nlohmann::json& document);
[[nodiscard]] plugin_manifest load_plugin_manifest(const std::filesystem::path& path);
[[nodiscard]] nlohmann::json plugin_manifest_to_json(const plugin_manifest& manifest);

} // namespace wuwe::runtime_host

#endif // WUWE_RUNTIME_HOST_PLUGIN_MANIFEST_HPP
