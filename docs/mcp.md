---
id: mcp
title: Model Context Protocol
description: Build MCP servers, clients, process hosts, gateways, and HTTP endpoints.
---

# Model Context Protocol

Wuwe implements MCP protocol version `2024-11-05` for exposing tools and context, consuming external servers, and aggregating multiple process servers behind one host.

## Server

`mcp_server` supports:

- initialization and capability negotiation;
- tools, resources, resource templates, prompts, and roots;
- pagination;
- resource subscriptions and list-change notifications;
- logging and progress notifications;
- server-initiated sampling and elicitation requests when the client advertises them;
- request lifecycle tracking, timeouts, access policy, authentication context, and audit callbacks.

## Minimal stdio server

```cpp
struct echo_text {
  static constexpr std::string_view description = "Echo a text string.";
  std::string text;

  std::string invoke() const { return text; }
};

int main() {
  wuwe::tool_provider<echo_text> tools;

  wuwe::agent::mcp::mcp_server server({
    .name = "wuwe-example",
    .version = "0.1.0",
  });
  server.add_tool_provider(tools);

  wuwe::agent::mcp::mcp_stdio_transport transport;
  return transport.run_stdio(server);
}
```

The stdio transport accepts Content-Length framing and newline-delimited JSON-RPC, and responds with the framing used by the incoming request. Keep stdout reserved for protocol messages.

Resources, templates, prompts, image content, and typed tools are demonstrated in `examples/src/mcp_stdio_example.cpp`.

## Clients and process hosting

| Component | Responsibility |
| --- | --- |
| `mcp_stdio_client` | Client operations over caller-provided streams |
| `mcp_process_client` | Start and communicate with one MCP subprocess |
| `mcp_host_runtime` | Manage multiple process servers, config, health, restart, circuit breaking, async calls, snapshots, and events |
| `mcp_gateway` | Re-export tools, resources, and prompts from running hosted servers with namespaced identifiers |

The host runtime can load server definitions from JSON files, initialize processes, call protocol methods, collect stderr, and expose operational snapshots. The host application owns the user interface, secret handling, process allowlist, and configuration distribution.

## HTTP

`mcp_http_transport` adapts JSON-RPC requests to an MCP server. `mcp_http_listener` provides a cpp-httplib listener with configurable host, port, MCP path, health path, body limit, CORS policy, and authorization callback.

The listener binds to `127.0.0.1` by default and supports an ephemeral port. Treat broader network exposure as an application security decision; add authentication and transport security outside or around the listener as required.

## Security and telemetry

`mcp_access_policy` can restrict tools and resources. `mcp_auth_context` carries host authentication data, and audit callbacks record access decisions.

Host events can be sent to:

- in-memory storage;
- JSON Lines;
- Prometheus text metrics;
- OpenTelemetry-style spans;
- fan-out sinks;
- the common Wuwe agent event sink.

## Knowledge integration

`knowledge_tool_provider` can be registered with `mcp_server` to expose `search_knowledge`. The example `knowledge_mcp_example.cpp` combines a scoped knowledge retriever, an MCP tool, and a cited resource.

See [Host compatibility](mcp-host-compatibility.md) for framing and configuration guidance.
