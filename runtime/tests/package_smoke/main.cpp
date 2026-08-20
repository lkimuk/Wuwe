#include <wuwe/runtime_host.hpp>
#include <wuwe/version.hpp>

int main() {
  wuwe::runtime_host::host host;
  wuwe::runtime_host::plugin_catalog plugins;
  wuwe::runtime_host::plugin_runtime plugin_runtime;
  wuwe::runtime_host::plugin_tool_provider plugin_tools(plugin_runtime, {});
  wuwe::runtime_host::plugin_activation_options activation;
  wuwe::runtime_host::plugin_process_launch_options launch;
  wuwe::runtime_host::runtime_config config;
  wuwe::runtime_host::runtime_runner_context runner_context;
  const auto state = host.lifecycle_status();
  return state.state == wuwe::runtime_host::lifecycle_state::created &&
             plugins.snapshot().empty() && plugin_runtime.catalog_snapshot().empty() &&
             plugin_tools.descriptors().empty() && activation.environment.empty() &&
             launch.environment.empty() && config.plugins.packages.empty() &&
             !runner_context.tool_provider && wuwe::framework_version_major == 1
           ? 0
           : 1;
}
