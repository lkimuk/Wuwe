#include <wuwe/runtime_host/plugin_package.hpp>

bool wuwe_runtime_plugin_package_header_probe() {
  return !wuwe::runtime_host::plugin_manifest_filename.empty();
}
