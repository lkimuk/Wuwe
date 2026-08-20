#ifndef WUWE_RUNTIME_HOST_PLUGIN_CATALOG_HPP
#define WUWE_RUNTIME_HOST_PLUGIN_CATALOG_HPP

#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <wuwe/runtime_host/plugin_package.hpp>

namespace wuwe::runtime_host {

enum class plugin_registration_policy {
  reject_conflict,
  replace,
};

enum class plugin_registration_status {
  inserted,
  unchanged,
  replaced,
  conflict,
};

[[nodiscard]] const char* to_string(plugin_registration_status status) noexcept;

struct plugin_registration_result {
  plugin_registration_status status { plugin_registration_status::conflict };
  plugin_package_ptr package;
  plugin_package_ptr previous;

  [[nodiscard]] explicit operator bool() const noexcept {
    return status != plugin_registration_status::conflict;
  }
};

class plugin_catalog_snapshot final {
public:
  plugin_catalog_snapshot();

  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] bool empty() const noexcept {
    return size() == 0;
  }
  [[nodiscard]] plugin_package_ptr find(const std::string& id) const noexcept;
  [[nodiscard]] std::vector<plugin_package_ptr> providing(std::string_view capability) const;
  [[nodiscard]] std::vector<plugin_package_ptr> packages() const;

private:
  using package_map = std::map<std::string, plugin_package_ptr>;

  friend class plugin_catalog;
  explicit plugin_catalog_snapshot(std::shared_ptr<const package_map> packages);
  std::shared_ptr<const package_map> packages_;
};

class plugin_catalog final {
public:
  plugin_catalog();

  [[nodiscard]] plugin_registration_result register_package(plugin_package_ptr package,
    plugin_registration_policy policy = plugin_registration_policy::reject_conflict);
  [[nodiscard]] bool unregister_package(const std::string& id);
  [[nodiscard]] plugin_catalog_snapshot snapshot() const;

private:
  mutable std::mutex mutex_;
  std::shared_ptr<const plugin_catalog_snapshot::package_map> packages_;
};

} // namespace wuwe::runtime_host

#endif // WUWE_RUNTIME_HOST_PLUGIN_CATALOG_HPP
