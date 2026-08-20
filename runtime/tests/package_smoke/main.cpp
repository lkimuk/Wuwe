#include <wuwe/runtime_host.hpp>
#include <wuwe/version.hpp>

int main() {
  wuwe::runtime_host::host host;
  wuwe::runtime_host::plugin_catalog plugins;
  wuwe::runtime_host::plugin_runtime plugin_runtime;
  wuwe::runtime_host::plugin_tool_provider plugin_tools(plugin_runtime, {});
  const auto state = host.lifecycle_status();
  return state.state == wuwe::runtime_host::lifecycle_state::created &&
             plugins.snapshot().empty() &&
             plugin_runtime.catalog_snapshot().empty() &&
             plugin_tools.descriptors().empty() &&
             wuwe::framework_version_major == 1
           ? 0
           : 1;
}
