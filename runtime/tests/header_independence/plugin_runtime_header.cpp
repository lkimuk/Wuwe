#include <wuwe/runtime_host/plugin_runtime.hpp>

void plugin_runtime_header_is_self_contained() {
  const wuwe::runtime_host::plugin_runtime_options options;
  (void)options;
}
