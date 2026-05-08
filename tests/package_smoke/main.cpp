#include <wuwe/agent/mcp/mcp_async.hpp>
#include <wuwe/agent/mcp/mcp_host_runtime.hpp>
#include <wuwe/agent/mcp/mcp_host_telemetry.hpp>
#include <wuwe/agent/mcp/mcp_gateway.hpp>
#include <wuwe/agent/mcp/mcp_http_listener.hpp>
#include <wuwe/agent/mcp/mcp_process_client.hpp>
#include <wuwe/agent/mcp/mcp_server.hpp>
#include <wuwe/wuwe.h>

int main() {
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
