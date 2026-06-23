#include <chrono>
#include <string>
#include <utility>

#include <wuwe/wuwe.h>

int main() {
  wuwe::agent::execution::controlled_process_backend_config config;
  config.validate_python_on_start = false;
  config.python_startup_timeout = std::chrono::milliseconds(3000);

  auto probe = wuwe::agent::execution::probe_python_interpreter({
    .interpreter = {},
    .timeout = std::chrono::milliseconds(100),
  });
  if (probe.status != wuwe::agent::execution::python_interpreter_status::empty_path ||
      wuwe::agent::execution::to_string(probe.status) != "empty_path" ||
      probe.metadata["error_code"] != "python_interpreter_empty_path") {
    return 1;
  }

  wuwe::agent::execution::execution_policy policy;
  policy.max_limits.max_code_bytes = 65536;
  auto backend = wuwe::agent::execution::make_controlled_process_backend();
  wuwe::agent::execution::execution_runtime runtime(std::move(backend), policy);
  wuwe::agent::execution::execution_tool_provider execution_tools(runtime);
  if (execution_tools.tools().empty()) {
    return 1;
  }

  auto registry =
    wuwe::agent::execution::make_default_execution_backend_registry();
  auto restricted_descriptor =
    wuwe::agent::execution::restricted_process_backend_descriptor();
  if (restricted_descriptor.available ||
      restricted_descriptor.name != "restricted_process") {
    return 1;
  }
  wuwe::agent::execution::restricted_process_backend_config restricted_config;
  if (!restricted_config.deny_network || !restricted_config.use_job_object ||
      restricted_config.inherit_parent_environment ||
      !restricted_config.cleanup_runtime_staging ||
      wuwe::agent::execution::to_string(restricted_config.runtime_staging) !=
        std::string("copy_minimal_python_runtime")) {
    return 1;
  }
  wuwe::agent::execution::execution_backend_requirements requirements;
  requirements.require_timeout = true;
  if (registry.select_backend_name(requirements) != "controlled_process") {
    return 1;
  }
  requirements.require_filesystem_read_deny = true;
  if (registry.create("restricted_process") != nullptr) {
    return 1;
  }
  if (registry.create_best(requirements) != nullptr) {
    return 1;
  }

  wuwe::agent::mcp::mcp_server server;
  wuwe::agent::mcp::mcp_http_listener_options listener_options;
  listener_options.port = 0;
  wuwe::agent::mcp::mcp_http_listener listener(server, listener_options);
  server.add_root({ .uri = "file:///tmp/project", .name = "Project" });
  auto response = server.handle_message(R"({
    "jsonrpc":"2.0",
    "id":1,
    "method":"roots/list"
  })");
  return response.has_value() ? 0 : 1;
}
