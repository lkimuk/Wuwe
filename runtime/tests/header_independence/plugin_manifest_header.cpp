#include <wuwe/runtime_host/plugin_manifest.hpp>

int wuwe_runtime_plugin_manifest_header_probe() {
  return wuwe::runtime_host::plugin_manifest_schema_version;
}
