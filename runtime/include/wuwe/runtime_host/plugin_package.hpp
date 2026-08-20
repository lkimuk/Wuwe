#ifndef WUWE_RUNTIME_HOST_PLUGIN_PACKAGE_HPP
#define WUWE_RUNTIME_HOST_PLUGIN_PACKAGE_HPP

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include <wuwe/runtime_host/plugin_manifest.hpp>

namespace wuwe::runtime_host {

inline constexpr std::string_view plugin_manifest_filename = "wuwe-plugin.json";

enum class plugin_package_error_code {
  invalid_root,
  manifest_unavailable,
  manifest_too_large,
  manifest_invalid,
  entrypoint_unavailable,
  unsafe_path,
  unsupported_file,
  file_changed,
  filesystem_error,
};

[[nodiscard]] const char* to_string(plugin_package_error_code code) noexcept;

class plugin_package_error : public std::runtime_error {
public:
  plugin_package_error(plugin_package_error_code code, std::string message);

  [[nodiscard]] plugin_package_error_code code() const noexcept {
    return code_;
  }

private:
  plugin_package_error_code code_;
};

class plugin_package final {
public:
  [[nodiscard]] const plugin_manifest& manifest() const noexcept {
    return manifest_;
  }
  [[nodiscard]] const std::filesystem::path& root() const noexcept {
    return root_;
  }
  [[nodiscard]] const std::filesystem::path& manifest_path() const noexcept {
    return manifest_path_;
  }
  [[nodiscard]] const std::filesystem::path& entrypoint_path() const noexcept {
    return entrypoint_path_;
  }
  [[nodiscard]] const std::string& content_sha256() const noexcept {
    return content_sha256_;
  }

private:
  friend std::shared_ptr<const plugin_package> load_plugin_package(
    const std::filesystem::path& root);

  plugin_package(plugin_manifest manifest, std::filesystem::path root,
    std::filesystem::path manifest_path, std::filesystem::path entrypoint_path,
    std::string content_sha256);

  plugin_manifest manifest_;
  std::filesystem::path root_;
  std::filesystem::path manifest_path_;
  std::filesystem::path entrypoint_path_;
  std::string content_sha256_;
};

using plugin_package_ptr = std::shared_ptr<const plugin_package>;

[[nodiscard]] plugin_package_ptr load_plugin_package(const std::filesystem::path& root);

} // namespace wuwe::runtime_host

#endif // WUWE_RUNTIME_HOST_PLUGIN_PACKAGE_HPP
