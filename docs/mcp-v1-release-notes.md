# MCP v1 Release Notes

Date: 2026-05-08

The MCP module is ready as a product-grade v1 module for Wuwe's no-UI Agent
framework. It provides protocol, transport, host-runtime, gateway, security,
observability, RAG exposure, diagnostics, examples, and release validation
coverage for framework integration.

## Included

- JSON-RPC MCP server surface for tools, resources, resource templates, roots,
  prompts, ping, initialization, notifications, sampling, and elicitation.
- Stdio server transport with Content-Length framing and JSON Lines compatibility
  for local desktop MCP hosts.
- HTTP adapter for application-owned HTTP services.
- Optional local HTTP listener using checked-in `cpp-httplib`, with `POST /mcp`,
  `GET /healthz`, auth callback, optional CORS, localhost default binding, and
  request body limits.
- Stdio client and external process client for talking to downstream MCP
  servers.
- Multi-server host runtime with config loading, process lifecycle, async
  dispatch, health checks, restart policy, backoff, circuit breaker, snapshots,
  stderr capture, structured events, and telemetry export.
- Gateway projection for aggregating downstream tools, resources, and prompts
  behind a single local `mcp_server`.
- Access policy, audit sink, sensitive argument redaction, tenant/scope metadata,
  and host-injected authentication context.
- Request lifecycle and async task registries with progress, timeout, polling,
  cooperative cancellation, and cleanup.
- Config diagnostics for user-provided MCP host configs before launching child
  processes.
- RAG exposure through `knowledge_mcp_example` and the `search_knowledge` tool.
- Package smoke coverage for installed headers and linkable MCP APIs, including
  the built-in HTTP listener.

## Validation

- Debug library build: pass.
- Debug `mcp_tests`: pass.
- Debug host transcript: pass when run outside sandbox.
- Debug `ctest`: pass.
- Debug install and package smoke: pass.
- Release `tools/mcp-release-check.ps1`: pass.

The Debug full release script could not rebuild the active example executables
while VS Code MCP had `mcp_stdio_example.exe` and `knowledge_mcp_example.exe`
running. The equivalent Debug validation steps were run without stopping those
active MCP services.

## Boundaries

- Cursor, Claude Desktop, and Continue remain pending real-client validation.
  They are not claimed as verified hosts in v1.
- The built-in HTTP listener is intentionally lightweight. Deployments that need
  TLS ownership, OAuth/JWT/JWKS verification, advanced CORS, reverse-proxy
  policy, rate limiting, or a full streaming event hub should put
  `mcp_http_transport` behind an application server or reverse proxy.
- There is no first-party UI/control plane by design for this v1 scope.
- Sampling and elicitation are exposed at the protocol layer; model choice and
  UI rendering remain host/application responsibilities.
- Cancellation is cooperative for user callbacks.
