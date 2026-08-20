#include <wuwe/runtime_host/plugin_catalog.hpp>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace wuwe::runtime_host {

const char* to_string(plugin_registration_status status) noexcept {
  switch (status) {
    case plugin_registration_status::inserted:
      return "inserted";
    case plugin_registration_status::unchanged:
      return "unchanged";
    case plugin_registration_status::replaced:
      return "replaced";
    case plugin_registration_status::conflict:
      return "conflict";
  }
  return "unknown";
}

plugin_catalog_snapshot::plugin_catalog_snapshot()
    : packages_(std::make_shared<const package_map>()) {
}

plugin_catalog_snapshot::plugin_catalog_snapshot(std::shared_ptr<const package_map> packages)
    : packages_(std::move(packages)) {
}

std::size_t plugin_catalog_snapshot::size() const noexcept {
  return packages_->size();
}

plugin_package_ptr plugin_catalog_snapshot::find(const std::string& id) const noexcept {
  const auto found = packages_->find(id);
  return found == packages_->end() ? plugin_package_ptr {} : found->second;
}

std::vector<plugin_package_ptr> plugin_catalog_snapshot::providing(
  std::string_view capability) const {
  std::vector<plugin_package_ptr> result;
  for (const auto& [_, package] : *packages_) {
    const auto& capabilities = package->manifest().capabilities;
    if (std::find(capabilities.begin(), capabilities.end(), capability) != capabilities.end())
      result.push_back(package);
  }
  return result;
}

std::vector<plugin_package_ptr> plugin_catalog_snapshot::packages() const {
  std::vector<plugin_package_ptr> result;
  result.reserve(packages_->size());
  for (const auto& [_, package] : *packages_)
    result.push_back(package);
  return result;
}

plugin_catalog::plugin_catalog()
    : packages_(std::make_shared<const plugin_catalog_snapshot::package_map>()) {
}

plugin_registration_result plugin_catalog::register_package(
  plugin_package_ptr package, plugin_registration_policy policy) {
  if (!package)
    throw std::invalid_argument("cannot register a null plugin package");
  switch (policy) {
    case plugin_registration_policy::reject_conflict:
    case plugin_registration_policy::replace:
      break;
    default:
      throw std::invalid_argument("unknown plugin registration policy");
  }
  const auto id = package->manifest().id;
  if (id.empty())
    throw std::invalid_argument("cannot register a plugin package with an empty id");

  std::scoped_lock lock(mutex_);
  const auto found = packages_->find(id);
  const auto existing = found == packages_->end() ? plugin_package_ptr {} : found->second;
  if (existing && (existing == package || existing->content_sha256() == package->content_sha256())) {
    return {
      .status = plugin_registration_status::unchanged,
      .package = existing,
    };
  }
  if (existing && policy == plugin_registration_policy::reject_conflict) {
    return {
      .status = plugin_registration_status::conflict,
      .package = std::move(package),
      .previous = existing,
    };
  }
  auto next = std::make_shared<plugin_catalog_snapshot::package_map>(*packages_);
  (*next)[id] = std::move(package);
  const auto registered = next->at(id);
  packages_ = std::shared_ptr<const plugin_catalog_snapshot::package_map>(std::move(next));
  return {
    .status = existing ? plugin_registration_status::replaced
                       : plugin_registration_status::inserted,
    .package = registered,
    .previous = existing,
  };
}

bool plugin_catalog::unregister_package(const std::string& id) {
  std::scoped_lock lock(mutex_);
  if (!packages_->contains(id))
    return false;
  auto next = std::make_shared<plugin_catalog_snapshot::package_map>(*packages_);
  next->erase(id);
  packages_ = std::shared_ptr<const plugin_catalog_snapshot::package_map>(std::move(next));
  return true;
}

plugin_catalog_snapshot plugin_catalog::snapshot() const {
  std::scoped_lock lock(mutex_);
  return plugin_catalog_snapshot(packages_);
}

} // namespace wuwe::runtime_host
