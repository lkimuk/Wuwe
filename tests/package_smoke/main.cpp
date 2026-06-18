#include <utility>

#include <wuwe/wuwe.h>

int main() {
  wuwe::agent::execution::execution_policy policy;
  policy.max_limits.max_code_bytes = 65536;
  auto backend = wuwe::agent::execution::make_controlled_process_backend();
  wuwe::agent::execution::execution_runtime runtime(std::move(backend), policy);
  wuwe::agent::execution::execution_tool_provider execution_tools(runtime);
  if (execution_tools.tools().empty()) {
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
