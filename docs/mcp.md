# MCP Module

`wuwe::agent::mcp` exposes Wuwe tools through a small Model Context Protocol
server surface. The module is intentionally independent from memory and RAG:
anything that implements the existing provider shape can be registered.

Implemented in v1:

- JSON-RPC 2.0 request parsing and error responses.
- JSON-RPC batch request handling.
- `initialize` with tools capability advertisement.
- Client `initialize` metadata capture and `notifications/initialized` state
  tracking.
- `ping` health checks.
- `tools/list` with Wuwe `llm_tool` names, descriptions, and JSON schemas.
- `tools/call` with MCP text content results.
- Direct MCP tools can return text, image, audio, blob-style, or resource-link
  content through `mcp_content`.
- `resources/list`, `resources/read`, and `resources/templates/list` through
  explicit resource registration.
- `roots/list` through host-injected roots for workspace boundary discovery.
- `prompts/list` and `prompts/get` through explicit prompt registration.
- Server-initiated `sampling/createMessage` and `elicitation/create` requests
  that host transports can forward to MCP clients.
- Content-Length framed stdio transport for local MCP hosts.
- Line-delimited stdio helper for simple manual debugging.
- Provider bridge through `mcp_server::add_tool_provider(provider)`.
- Optional access policy and audit sink for tools, resources, resource
  templates, and prompts.
- Server-side log and progress notifications, plus client cancellation
  notification tracking.
- Optional cursor pagination for list methods through `set_list_page_size()`.
- Basic stdio MCP client for sending framed requests and notifications over
  caller-provided streams.
- Resource subscribe/unsubscribe plus resource/tool/prompt listChanged
  notifications.
- HTTP adapter for application-owned HTTP servers, with emitted notifications
  exposed as SSE event payloads.
- Request lifecycle registry for tool calls, resource reads, and prompt gets.
- Request timeout and per-request progress tracking for executable requests.
- Optional async task registry with cooperative cancellation, polling, progress,
  timeout state, and finished-task cleanup.

## Current Status

The MCP module is currently at a product-grade foundation for the library-side
server surface. The server, transports, lifecycle helpers, security hooks,
examples, documentation, and release checks are implemented and covered by
Debug and Release validation.

Completed work:

- Core MCP server methods for initialization, health checks, tools, resources,
  resource templates, roots, prompts, sampling, and elicitation.
- Rich MCP content results for text, images, audio, blob-style payloads, and
  resource links.
- Wuwe tool-provider bridging through `add_tool_provider()` without requiring
  tools to hand-write MCP names.
- MCP-native tool registration through `add_mcp_tool()` for richer result
  shapes.
- Content-Length framed stdio transport, including Windows binary stdio setup
  so CRLF framing is not corrupted by text-mode translation.
- Application-owned HTTP adapter with JSON-RPC responses, notification-only
  handling, HTTP status mapping, and SSE event payload generation.
- Small synchronous stdio client for caller-owned streams.
- Access policy, tenant/scope metadata, audit sink integration, sensitive
  argument redaction, and host-injected authentication context.
- Request lifecycle registry with state, params, error, timeout, progress token,
  progress value, optional total, and timing metadata.
- Async task registry with cooperative cancellation, progress, timeout
  detection, polling, snapshots, and finished-task cleanup.
- Resource subscription, resource update notifications, and list-changed
  notifications for tools, resources, roots, and prompts.
- Typed C++ helpers for sampling and elicitation request/result payloads.
- `mcp_stdio_example` for general host compatibility.
- `knowledge_mcp_example` for exposing the RAG `search_knowledge` tool through
  MCP.
- Project-level VS Code MCP configuration in `.vscode/mcp.json`.
- External package smoke test through `find_package(wuwe CONFIG REQUIRED)`.
- Release validation script that builds, runs `mcp_tests`, runs a real process
  stdio transcript, runs `ctest`, installs the package, and runs package smoke
  for Debug and Release.

Next tasks:

- Run the checked-in VS Code MCP workspace config in the VS Code UI and record
  observed tool/resource/prompt visibility in
  [MCP Host Compatibility](mcp-host-compatibility.md).
- Run the same host validation against Cursor, Claude Desktop, and Continue
  when those hosts are installed and available.
- Decide whether Wuwe should stay focused on MCP server/library support or also
  become a full MCP host/runtime.
- If Wuwe should become a host/runtime, add child process management for
  launching and supervising external MCP servers.
- If Wuwe should expose MCP over the network directly, add a built-in HTTP/SSE
  listener; otherwise keep the existing application-owned HTTP adapter.
- If Wuwe should own identity verification, add OAuth/JWT/JWKS validation;
  otherwise continue injecting authenticated identity through `mcp_auth_context`.
- Add real host regression notes whenever a host changes behavior or requires a
  host-specific configuration shape.

Example:

```cpp
#include <string>
#include <string_view>
#include <vector>

#include <wuwe/agent/mcp/mcp_server.hpp>
#include <wuwe/agent/mcp/mcp_stdio_transport.hpp>
#include <wuwe/agent/tools/tool.hpp>

struct echo_text {
  static constexpr std::string_view description = "Echo a text string.";
  std::string text;

  std::string invoke() const {
    return text;
  }
};

int main() {
  wuwe::tool_provider<echo_text> tools;
  wuwe::agent::mcp::mcp_server server;
  server.add_tool_provider(tools);
  server.add_resource(
    {
      .uri = "wuwe://example/readme",
      .name = "Example README",
      .description = "A small text resource.",
      .mime_type = "text/plain",
    },
    [] {
      return std::vector<wuwe::agent::mcp::mcp_resource_content> {
        {
          .uri = "wuwe://example/readme",
          .mime_type = "text/plain",
          .text = "Hello from a Wuwe MCP resource.",
        },
      };
    });
  server.add_prompt(
    {
      .name = "echo_prompt",
      .description = "Create a user message that echoes a topic.",
      .arguments = {
        {
          .name = "topic",
          .description = "Topic to echo.",
          .required = false,
        },
      },
    },
    [](const wuwe::agent::mcp::json& arguments) {
      return wuwe::agent::mcp::mcp_prompt_result {
        .description = "Echo prompt.",
        .messages = {
          {
            .role = "user",
            .text = "Echo this topic: " + arguments.value("topic", std::string("Wuwe MCP")),
          },
        },
      };
    });
  server.set_access_policy({
    .allowed_tools = { "echo_text" },
    .allowed_resources = { "wuwe://example/readme" },
    .allowed_resource_templates = { "wuwe://example/{name}" },
    .allowed_prompts = { "echo_prompt" },
  });
  server.set_audit_sink([](const wuwe::agent::mcp::mcp_audit_event& event) {
    // Store event.action, event.target, event.allowed, event.reason, and
    // event.arguments in the host application's audit log.
  });

  wuwe::agent::mcp::mcp_stdio_transport transport;
  return transport.run_stdio(server);
}
```

Build and test:

```powershell
cmake --build build --config Debug --target mcp_tests mcp_stdio_example
.\build\tests\Debug\mcp_tests.exe
```

For real desktop host setup and validation, see
[MCP Host Compatibility](mcp-host-compatibility.md).

The default stdio transport uses MCP-style `Content-Length` framing:

```text
Content-Length: 86

{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05"}}
```

After the client sends `notifications/initialized`, `server.initialized()`
returns true. `initialize` also records `clientInfo` and `capabilities` for host
introspection through `client_info()` and `client_capabilities()`.

Use `mcp_stdio_transport::run_lines()` only for manual line-delimited debugging.
For example:

```json
{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05"}}
```

and:

```json
{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"echo_text","arguments":{"text":"hello"}}}
```

Resources:

```json
{"jsonrpc":"2.0","id":3,"method":"resources/list"}
```

```json
{"jsonrpc":"2.0","id":4,"method":"resources/read","params":{"uri":"wuwe://example/readme"}}
```

```json
{"jsonrpc":"2.0","id":5,"method":"resources/subscribe","params":{"uri":"wuwe://example/readme"}}
```

Roots:

```json
{"jsonrpc":"2.0","id":6,"method":"roots/list"}
```

Prompts:

```json
{"jsonrpc":"2.0","id":7,"method":"prompts/list"}
```

```json
{"jsonrpc":"2.0","id":8,"method":"prompts/get","params":{"name":"echo_prompt","arguments":{"topic":"tools"}}}
```

## Pagination

By default, list methods return every visible item. Configure a page size to
enable cursor pagination for `tools/list`, `resources/list`,
`resources/templates/list`, `roots/list`, and `prompts/list`:

```cpp
server.set_list_page_size(50);
```

When more items are available, the response includes `nextCursor`; pass it back
as `params.cursor`:

```json
{"jsonrpc":"2.0","id":7,"method":"tools/list","params":{"cursor":"50"}}
```

## Content

Reflected Wuwe tools registered through `add_tool_provider()` are exposed as
MCP text content because their existing result type is `llm_tool_result`.
For richer output, register a direct MCP tool with `add_mcp_tool()`:

```cpp
server.add_mcp_tool(
  {
    .name = "preview_image",
    .description = "Return an image preview.",
    .parameters_json_schema = R"({"type":"object","properties":{}})",
  },
  [](const wuwe::agent::mcp::json&) {
    return wuwe::agent::mcp::mcp_tool_call_result {
      .content = {
        wuwe::agent::mcp::mcp_content::image("iVBORw0KGgo="),
      },
    };
  });
```

Resources can return text or base64 `blob` payloads. Prompt messages can use
plain `.text` or a richer `.content` value. Prefer helper factories such as
`mcp_content::text_content()`, `mcp_content::image()`,
`mcp_resource_content::text_content()`, `mcp_resource_content::blob_content()`,
and `mcp_prompt_message::user_text()` for stable call sites.

## Notifications

Direct MCP tools can emit log and progress notifications while they run:

```cpp
server.add_mcp_tool(
  {
    .name = "long_task",
    .description = "Run a long task.",
    .parameters_json_schema = R"({"type":"object","properties":{}})",
  },
  [&server](const wuwe::agent::mcp::json&) {
    server.emit_log("info", "long task started", "my-agent");
    server.emit_progress("task-1", 50.0, 100.0, "halfway");
    return wuwe::agent::mcp::mcp_tool_call_result {
      .content = { { .type = "text", .text = "done" } },
    };
  });
```

`mcp_stdio_transport` writes emitted notifications before the final response.
The server also tracks `notifications/cancelled`; check
`server.is_cancelled(request_id)` from long-running callbacks that choose to
cooperate with cancellation.

For dynamic registries, call `emit_tool_list_changed()`,
`emit_resource_list_changed()`, `emit_roots_list_changed()`, or
`emit_prompt_list_changed()`. Resources can also be subscribed to with
`resources/subscribe`; call
`emit_resource_updated(uri)` to notify subscribed clients.

## Roots

Use roots to expose host-approved workspace boundaries to MCP tools and
adapters. The server does not scan the filesystem; applications inject roots
that have already passed their own policy checks:

```cpp
server.set_roots({
  { .uri = "file:///workspace/project", .name = "Project" },
});
auto roots = server.roots();
```

`roots/list` returns the registered roots and supports the same cursor
pagination as other list methods. `mcp_stdio_client::list_roots()` sends a
framed `roots/list` request when Wuwe is consuming another MCP endpoint.

## Sampling And Elicitation

Sampling and elicitation are server-initiated JSON-RPC requests. Wuwe does not
pick a model or render a UI here; it creates protocol requests and lets the MCP
host decide whether and how to fulfill them.

After `initialize`, the server only enqueues sampling or elicitation requests
when the client declared the matching capability. Before an MCP handshake,
embedded callers may still enqueue them for direct test harnesses or custom
host wiring.

Inside a tool, resource, or prompt callback, enqueue requests with raw MCP
params:

```cpp
const auto sampling_id = server.request_sampling({
  { "messages", wuwe::agent::mcp::json::array({
    {
      { "role", "user" },
      { "content", { { "type", "text" }, { "text", "Draft a title." } } },
    },
  }) },
  { "maxTokens", 64 },
});

const auto elicitation_id = server.request_elicitation({
  { "message", "Choose a project name." },
  { "requestedSchema", {
    { "type", "object" },
    { "properties", {
      { "name", { { "type", "string" } } },
    } },
  } },
});
```

Typed helpers are also available when callers want stable C++ construction and
parsing:

```cpp
const auto sampling_id = server.request_sampling(
  wuwe::agent::mcp::mcp_sampling_request {
    .messages = {
      wuwe::agent::mcp::mcp_sampling_message::user_text("Draft a title."),
    },
    .max_tokens = 64,
    .temperature = 0.2,
  });

const auto result = wuwe::agent::mcp::mcp_sampling_result::from_json(record->result);
```

`handle_message_exchange()` returns these outbound requests in
`exchange.requests`. `mcp_stdio_transport` writes them as Content-Length framed
JSON-RPC requests before notifications and the final response. `mcp_http_transport`
exposes them as `response.client_requests`, because the application-owned HTTP
framework decides how to deliver them to the client.

When the host later sends a JSON-RPC response with the same id, `mcp_server`
records the result or error:

```cpp
auto record = server.client_request(sampling_id);
server.clear_completed_client_requests();
```

## Threading

`mcp_server` protects its mutable protocol/session state with an internal
recursive mutex. This allows callbacks to call APIs such as `emit_log()`,
`emit_request_progress()`, `request_sampling()`, and `request_elicitation()`
while the server is handling a request. The synchronous callback itself still
runs on the caller's thread; hosts that want parallel execution should use a
server instance per session or place long-running work behind
`mcp_async_task_registry`.

The lifecycle registry and async task registry also protect their own state.
Tool, resource, and prompt callback implementations are responsible for their
own shared data.

## Request Lifecycle

`mcp_server` records executable requests in `request_registry()`:

```cpp
auto record = server.request_registry().get("42");
```

Records include method, target, params, state, error, progress token, progress,
optional total, progress message, timeout, and start/finish times. The registry
currently tracks `tools/call`, `resources/read`, and `prompts/get`.

Use `set_request_timeout()` to mark synchronous executable requests as failed if
their callback returns after the configured duration:

```cpp
server.set_request_timeout(std::chrono::milliseconds(5000));
```

Long-running callbacks can update the lifecycle record and emit a matching MCP
progress notification with `emit_request_progress()`:

```cpp
server.emit_request_progress(42, "task-42", 50.0, 100.0, "halfway");
```

For host-managed asynchronous work, use `mcp_async_task_registry`. It owns
`std::async` tasks, records lifecycle snapshots, supports cooperative
cancellation tokens, records progress, marks timeouts, and only clears finished
tasks once their future is ready:

```cpp
wuwe::agent::mcp::mcp_async_task_registry tasks;
tasks.submit(
  "job-1",
  "tools/call",
  "long_task",
  wuwe::agent::mcp::json::object(),
  [&tasks](wuwe::agent::mcp::mcp_async_cancel_token token) {
    if (token.is_cancelled()) {
      return;
    }
    tasks.progress("job-1", "progress-1", 50.0, 100.0, "halfway");
  },
  std::chrono::seconds(30));

auto snapshot = tasks.poll("job-1");
tasks.cancel("job-1", "client requested cancellation");
tasks.clear_finished();
```

Cancellation is intentionally cooperative. C++ code should check
`mcp_async_cancel_token::is_cancelled()` at natural interruption points.

## Stdio Client

`mcp_stdio_client` is a small synchronous client over caller-owned streams. It
does not launch child processes; host applications own process management and
pass the connected streams in:

```cpp
wuwe::agent::mcp::mcp_stdio_client client(input, output);
auto initialized = client.initialize({ .name = "my-host", .version = "1.0.0" });
client.notify("notifications/initialized");
auto pong = client.ping();
auto tools = client.list_tools();
auto result = client.call_tool("echo_text", { { "text", "hello" } });
auto resources = client.list_resources();
auto resource = client.read_resource("wuwe://example/readme");
auto roots = client.list_roots();
auto prompts = client.list_prompts();
auto prompt = client.get_prompt("echo_prompt", { { "topic", "tools" } });
```

Notifications received while waiting for a response are stored in
`client.notifications()`.

## HTTP Adapter

`mcp_http_transport` adapts an application-owned HTTP endpoint to `mcp_server`.
It does not start a web server, bind sockets, own threads, or manage TLS.
Applications keep listener ownership and route framework requests into the
adapter:

```cpp
wuwe::agent::mcp::mcp_http_transport transport;
auto response = transport.handle(server, {
  .method = "POST",
  .body = request_body,
  .headers = request_headers,
});
```

The adapter accepts `POST` with an optional `application/json` content type.
Unsupported methods return `405` with `Allow: POST`, non-JSON content types
return `415`, and empty POST bodies return `400`. JSON-RPC parse and method
errors stay in the JSON-RPC response body.

`response.body` contains the JSON-RPC response when there is one. Notification
only requests return status `202`. Server notifications emitted while handling a
request are available in `response.sse_events` as preformatted SSE event strings;
use `response.sse_body()` when a framework expects a single SSE response body.

## Access Policy

By default, a server allows every registered tool, resource, resource template,
and prompt. Configure `mcp_access_policy` to narrow the surface:

```cpp
wuwe::agent::mcp::mcp_access_policy policy;
policy.tenant_id = "tenant-a";
policy.scopes = { "tools:call" };
policy.allowed_tools = { "search_knowledge" };
policy.denied_resources = { "wuwe://private/session" };
server.set_access_policy(policy);
```

Deny lists take precedence over allow lists. If an allow list for a category is
non-empty, only those names or URIs are listed and callable/readable. Set
`allow_unlisted = false` to require explicit allow lists for every category.
Audit events include the configured `tenant_id` and `scopes`.

Use `set_audit_sink()` to observe `tools/call`, `resources/read`, and
`prompts/get` decisions. Audit arguments are recursively redacted for sensitive
keys before they reach the sink. Defaults include `api_key`, `apikey`,
`authorization`, `password`, `secret`, and `token`; override
`redacted_argument_keys` when a host needs a stricter schema. Denied calls return
JSON-RPC error `-32001`.

Use `set_auth_context()` to attach host-authenticated identity metadata to the
server session. Wuwe does not implement OAuth itself at this layer; HTTP or
stdio hosts authenticate externally and inject the resulting subject, issuer,
audience, scopes, and claims:

```cpp
server.set_auth_context({
  .subject = "user-123",
  .issuer = "https://issuer.example",
  .audience = "wuwe-mcp",
  .scopes = { "tools:call" },
  .claims = { { "email", "user@example.test" } },
});
```

Audit events include this `mcp_auth_context`, so authorization and audit logs can
be correlated without changing tool callback signatures.

## RAG Over MCP

`knowledge_mcp_example` exposes the RAG `search_knowledge` tool through the MCP
stdio transport. It seeds a small local knowledge corpus, registers the
knowledge tool provider, and serves a resource that describes the example:

```powershell
cmake --build build --config Debug --target knowledge_mcp_example
.\build\examples\Debug\knowledge_mcp_example.exe
```

Use it from an MCP host the same way as `mcp_stdio_example`, with the absolute
path to `knowledge_mcp_example.exe`. The exposed tool is `search_knowledge`.

## API Reference

Core server:

- `mcp_server::add_tool()` registers an explicit `llm_tool` bridge.
- `mcp_server::add_tool_provider()` reflects an existing Wuwe tool provider.
- `mcp_server::add_mcp_tool()` registers a rich MCP-native tool callback.
- `mcp_server::add_resource()` and `add_resource_template()` register static
  resource entries and template metadata.
- `mcp_server::add_prompt()` registers prompt metadata and a prompt callback.
- `mcp_server::add_root()` and `set_roots()` register host-approved workspace
  roots.
- `mcp_server::handle_message()` handles one JSON-RPC payload.
- `mcp_server::handle_message_exchange()` returns outbound client requests,
  emitted notifications, and the optional final response.
- `mcp_server::request_sampling()` and `request_elicitation()` enqueue
  server-initiated MCP client requests when the client capability allows it.
- `mcp_server::client_request()` and `clear_completed_client_requests()` inspect
  and clean server-initiated request records.

Transports and clients:

- `mcp_stdio_transport::run_stdio()` serves Content-Length framed stdio.
- `mcp_stdio_transport::run()` serves caller-provided streams.
- `mcp_stdio_transport::run_lines()` is only for line-delimited debug harnesses.
- `mcp_stdio_client` sends framed requests over caller-owned streams.
- `mcp_http_transport::handle()` adapts application HTTP requests without
  owning a listener.

Operational helpers:

- `mcp_access_policy` defines allow/deny lists, tenant, scopes, and audit
  redaction keys.
- `mcp_request_registry` stores synchronous executable request lifecycle
  records.
- `mcp_async_task_registry` stores host-managed async task lifecycle snapshots.
- `mcp_content`, `mcp_resource_content`, and `mcp_prompt_message` provide stable
  helper factories for rich MCP payloads.

Current boundaries:

- No built-in HTTP listener or child process management yet.
- Sampling and elicitation are exposed as protocol requests; concrete model
  choice and UI rendering remain host responsibilities.
- No OAuth identity integration yet.
- Cancellation is cooperative; synchronous callbacks must check
  `is_cancelled()` themselves, and async callbacks should check their
  `mcp_async_cancel_token`.
