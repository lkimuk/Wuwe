#include <wuwe/runtime_host/plugin_manifest.hpp>

#include <algorithm>
#include <fstream>
#include <regex>
#include <set>
#include <string_view>
#include <utility>

#include "endpoint_validation.hpp"

namespace wuwe::runtime_host {
namespace {

using json = nlohmann::json;

void issue(validation_report& report, std::string path, std::string code, std::string message) {
  report.issues.push_back(
    { .path = std::move(path), .code = std::move(code), .message = std::move(message) });
}

void reject_unknown(const json& value, const std::set<std::string>& allowed, std::string_view path,
  validation_report& report) {
  if (!value.is_object())
    return;
  for (const auto& [key, _] : value.items()) {
    if (!allowed.contains(key)) {
      issue(report,
        std::string(path) + "/" + key,
        "unknown_field",
        "field is not part of plugin manifest schema version 1");
    }
  }
}

std::string safe_string(const json& value, const char* name) {
  const auto found = value.find(name);
  return found != value.end() && found->is_string() ? found->get<std::string>() : std::string {};
}

void require_string(
  const json& value, const char* name, std::string_view path, validation_report& report) {
  const auto found = value.find(name);
  if (found == value.end() || !found->is_string() || found->get_ref<const std::string&>().empty()) {
    issue(report, std::string(path) + "/" + name, "required", "value must be a non-empty string");
  }
}

void validate_string_array(
  const json& value, const char* name, std::string_view path, validation_report& report) {
  const auto found = value.find(name);
  if (found == value.end())
    return;
  if (!found->is_array()) {
    issue(report,
      std::string(path) + "/" + name,
      "invalid_type",
      "value must be an array of non-empty strings");
    return;
  }
  std::set<std::string> unique;
  for (std::size_t index = 0; index < found->size(); ++index) {
    const auto& item = found->at(index);
    const auto item_path = std::string(path) + "/" + name + "/" + std::to_string(index);
    if (!item.is_string() || item.get_ref<const std::string&>().empty()) {
      issue(report, item_path, "invalid_value", "entry must be a non-empty string");
      continue;
    }
    if (!unique.insert(item.get<std::string>()).second)
      issue(report, item_path, "duplicate", "duplicate entries are not allowed");
  }
}

bool safe_package_relative_path(std::string_view path) {
  if (path.empty() || path.front() == '/' || path.front() == '\\' || path.find(':') != path.npos)
    return false;
  std::size_t begin = 0;
  while (begin <= path.size()) {
    const auto end = path.find_first_of("/\\", begin);
    const auto component = path.substr(begin, end == path.npos ? path.size() - begin : end - begin);
    if (component.empty() || component == "." || component == "..")
      return false;
    if (end == path.npos)
      break;
    begin = end + 1;
  }
  return true;
}

bool valid_semantic_version_identifier(std::string_view identifier, bool prerelease) {
  if (identifier.empty())
    return false;
  const bool numeric = std::all_of(identifier.begin(), identifier.end(), [](unsigned char value) {
    return value >= '0' && value <= '9';
  });
  if (prerelease && numeric && identifier.size() > 1 && identifier.front() == '0')
    return false;
  return std::all_of(identifier.begin(), identifier.end(), [](unsigned char value) {
    return (value >= '0' && value <= '9') || (value >= 'A' && value <= 'Z') ||
           (value >= 'a' && value <= 'z') || value == '-';
  });
}

bool valid_semantic_version_suffix(std::string_view suffix, bool prerelease) {
  std::size_t begin = 0;
  while (begin <= suffix.size()) {
    const auto end = suffix.find('.', begin);
    const auto identifier =
      suffix.substr(begin, end == suffix.npos ? suffix.size() - begin : end - begin);
    if (!valid_semantic_version_identifier(identifier, prerelease))
      return false;
    if (end == suffix.npos)
      return true;
    begin = end + 1;
  }
  return false;
}

bool valid_semantic_version(std::string_view version) {
  static const std::regex core_pattern("^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)");
  std::match_results<std::string_view::const_iterator> match;
  if (!std::regex_search(version.begin(), version.end(), match, core_pattern) ||
      match.position() != 0)
    return false;
  auto suffix = version.substr(static_cast<std::size_t>(match.length()));
  if (suffix.empty())
    return true;
  if (suffix.front() == '-') {
    suffix.remove_prefix(1);
    const auto build = suffix.find('+');
    if (!valid_semantic_version_suffix(suffix.substr(0, build), true))
      return false;
    if (build == suffix.npos)
      return true;
    suffix.remove_prefix(build + 1);
  }
  else if (suffix.front() == '+') {
    suffix.remove_prefix(1);
  }
  else {
    return false;
  }
  return valid_semantic_version_suffix(suffix, false);
}

std::vector<std::string> string_array(const json& value, const char* name) {
  const auto found = value.find(name);
  return found == value.end() ? std::vector<std::string> {}
                              : found->get<std::vector<std::string>>();
}

std::vector<std::filesystem::path> path_array(const json& value, const char* name) {
  std::vector<std::filesystem::path> result;
  for (const auto& item : string_array(value, name))
    result.emplace_back(item);
  return result;
}

} // namespace

std::string to_string(plugin_entrypoint_kind kind) {
  switch (kind) {
    case plugin_entrypoint_kind::process:
      return "process";
  }
  return "unknown";
}

validation_report validate_plugin_manifest(const nlohmann::json& document) {
  validation_report report;
  if (!document.is_object()) {
    issue(report, "/", "invalid_type", "plugin manifest must be a JSON object");
    return report;
  }
  reject_unknown(document,
    { "schemaVersion",
      "id",
      "version",
      "runtimeApiVersion",
      "entrypoint",
      "capabilities",
      "permissions" },
    "",
    report);

  const auto schema = document.find("schemaVersion");
  if (schema == document.end() || !schema->is_number_integer()) {
    issue(report, "/schemaVersion", "required", "schemaVersion must be integer 1");
  }
  else if (schema->get<std::int64_t>() != plugin_manifest_schema_version) {
    issue(report,
      "/schemaVersion",
      "unsupported_version",
      "supported plugin manifest schema version is 1");
  }

  require_string(document, "id", "", report);
  require_string(document, "version", "", report);
  require_string(document, "runtimeApiVersion", "", report);
  const auto id = safe_string(document, "id");
  if (!id.empty() && !detail::valid_dns_name(id, true))
    issue(report, "/id", "invalid_format", "plugin id must use lowercase DNS-style characters");
  const auto version = safe_string(document, "version");
  if (!version.empty() && !valid_semantic_version(version))
    issue(report, "/version", "invalid_format", "plugin version must be semantic versioning");
  const auto api_version = safe_string(document, "runtimeApiVersion");
  if (!api_version.empty() && api_version != runtime_plugin_api_version) {
    issue(report,
      "/runtimeApiVersion",
      "incompatible_version",
      "plugin requires an unsupported Runtime API version");
  }

  const auto entrypoint = document.find("entrypoint");
  if (entrypoint == document.end() || !entrypoint->is_object()) {
    issue(report, "/entrypoint", "required", "entrypoint must be an object");
  }
  else {
    reject_unknown(*entrypoint, { "kind", "path" }, "/entrypoint", report);
    require_string(*entrypoint, "kind", "/entrypoint", report);
    require_string(*entrypoint, "path", "/entrypoint", report);
    if (safe_string(*entrypoint, "kind") != "process") {
      issue(report,
        "/entrypoint/kind",
        "unsupported_value",
        "runtime version 1 supports isolated process entrypoints only");
    }
    const auto path = safe_string(*entrypoint, "path");
    if (!path.empty()) {
      if (!safe_package_relative_path(path)) {
        issue(report,
          "/entrypoint/path",
          "unsafe_path",
          "plugin entrypoint must remain within the plugin package");
      }
    }
  }

  validate_string_array(document, "capabilities", "", report);
  if (const auto capabilities = document.find("capabilities");
      capabilities != document.end() && capabilities->is_array()) {
    static const std::regex capability_pattern("^[a-z][a-z0-9._-]*$");
    for (std::size_t index = 0; index < capabilities->size(); ++index) {
      const auto& value = capabilities->at(index);
      if (value.is_string() && !value.get_ref<const std::string&>().empty() &&
          !std::regex_match(value.get_ref<const std::string&>(), capability_pattern)) {
        issue(report,
          "/capabilities/" + std::to_string(index),
          "invalid_format",
          "capability names must use lowercase identifier characters");
      }
    }
  }
  const auto permissions = document.find("permissions");
  if (permissions != document.end()) {
    if (!permissions->is_object()) {
      issue(report, "/permissions", "invalid_type", "permissions must be an object");
    }
    else {
      reject_unknown(*permissions,
        { "environment", "readPaths", "writePaths", "networkHosts" },
        "/permissions",
        report);
      validate_string_array(*permissions, "environment", "/permissions", report);
      validate_string_array(*permissions, "readPaths", "/permissions", report);
      validate_string_array(*permissions, "writePaths", "/permissions", report);
      validate_string_array(*permissions, "networkHosts", "/permissions", report);
      if (const auto environment = permissions->find("environment");
          environment != permissions->end() && environment->is_array()) {
        static const std::regex environment_pattern("^[A-Za-z_][A-Za-z0-9_]*$");
        for (std::size_t index = 0; index < environment->size(); ++index) {
          const auto& value = environment->at(index);
          if (value.is_string() && !value.get_ref<const std::string&>().empty() &&
              !std::regex_match(value.get_ref<const std::string&>(), environment_pattern)) {
            issue(report,
              "/permissions/environment/" + std::to_string(index),
              "invalid_format",
              "environment permission must be a variable name");
          }
        }
      }
      if (const auto hosts = permissions->find("networkHosts");
          hosts != permissions->end() && hosts->is_array()) {
        for (std::size_t index = 0; index < hosts->size(); ++index) {
          const auto& value = hosts->at(index);
          if (!value.is_string() || value.get_ref<const std::string&>().empty())
            continue;
          const auto& endpoint = value.get_ref<const std::string&>();
          if (!detail::valid_network_endpoint(endpoint)) {
            issue(report,
              "/permissions/networkHosts/" + std::to_string(index),
              "invalid_format",
              "network permission must be an exact host with optional port");
          }
        }
      }
    }
  }
  return report;
}

plugin_manifest plugin_manifest_from_json(const nlohmann::json& document) {
  auto report = validate_plugin_manifest(document);
  if (!report)
    throw validation_error("plugin manifest", std::move(report));
  const auto& entrypoint = document.at("entrypoint");
  plugin_manifest manifest {
    .schema_version = document.at("schemaVersion").get<int>(),
    .id = document.at("id").get<std::string>(),
    .version = document.at("version").get<std::string>(),
    .runtime_api_version = document.at("runtimeApiVersion").get<std::string>(),
    .entrypoint_kind = plugin_entrypoint_kind::process,
    .entrypoint = entrypoint.at("path").get<std::string>(),
    .capabilities = string_array(document, "capabilities"),
  };
  if (const auto permissions = document.find("permissions"); permissions != document.end()) {
    manifest.permissions.environment = string_array(*permissions, "environment");
    manifest.permissions.read_paths = path_array(*permissions, "readPaths");
    manifest.permissions.write_paths = path_array(*permissions, "writePaths");
    manifest.permissions.network_hosts = string_array(*permissions, "networkHosts");
  }
  return manifest;
}

plugin_manifest load_plugin_manifest(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input)
    throw std::runtime_error("unable to open plugin manifest: " + path.string());
  try {
    nlohmann::json document;
    input >> document;
    return plugin_manifest_from_json(document);
  }
  catch (const nlohmann::json::exception& ex) {
    throw std::runtime_error(
      "unable to parse plugin manifest '" + path.string() + "': " + ex.what());
  }
}

nlohmann::json plugin_manifest_to_json(const plugin_manifest& manifest) {
  std::vector<std::string> read_paths;
  std::vector<std::string> write_paths;
  for (const auto& path : manifest.permissions.read_paths)
    read_paths.push_back(path.generic_string());
  for (const auto& path : manifest.permissions.write_paths)
    write_paths.push_back(path.generic_string());
  return {
    { "schemaVersion", manifest.schema_version },
    { "id", manifest.id },
    { "version", manifest.version },
    { "runtimeApiVersion", manifest.runtime_api_version },
    { "entrypoint",
      { { "kind", to_string(manifest.entrypoint_kind) },
        { "path", manifest.entrypoint.generic_string() } } },
    { "capabilities", manifest.capabilities },
    { "permissions",
      {
        { "environment", manifest.permissions.environment },
        { "readPaths", std::move(read_paths) },
        { "writePaths", std::move(write_paths) },
        { "networkHosts", manifest.permissions.network_hosts },
      } },
  };
}

} // namespace wuwe::runtime_host
