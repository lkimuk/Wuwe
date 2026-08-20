#include <wuwe/runtime_host/plugin_catalog.hpp>

bool wuwe_runtime_plugin_catalog_header_probe() {
  wuwe::runtime_host::plugin_catalog catalog;
  return catalog.snapshot().empty();
}
