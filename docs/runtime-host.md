# Wuwe Runtime Host

`wuwe-runtime` is the headless process host for Wuwe agents. It turns the Framework's model clients,
`llm_agent_runner`, durable run runtime, stores, approvals, and Host Protocol into an independently
deployable component. It does not duplicate agent execution logic from `wuwe::wuwe`.

## Architecture

The dependency direction is intentionally one-way:

```text
wuwe-runtime executable -> wuwe::runtime_host -> wuwe::wuwe
```

The CMake option `WUWE_BUILD_RUNTIME` controls the component. Installed packages that include it
export `wuwe::runtime_host` in addition to `wuwe::wuwe`, install the `wuwe-runtime` executable, and
support `find_package(wuwe 1 CONFIG REQUIRED COMPONENTS runtime_host)` for consumers that require the
component explicitly.

Runtime Host owns these process-level concerns:

- strict, versioned configuration;
- dependency assembly and readiness;
- bounded concurrent runs;
- cancellation, deadlines, bounded submission acknowledgement, draining, and graceful shutdown;
- in-memory or SQLite durable run storage;
- Host Protocol dispatch and idempotent mutations;
- health and configuration diagnostics;
- verified plugin package admission and snapshot-based catalog lookup;
- explicit process-plugin supervision over MCP stdio.

Agent algorithms, tools, memory, orchestration, and model provider implementations remain Framework
modules. This keeps embedded users from depending on the executable host.

## Configuration

Start from [`runtime/runtime.example.json`](../runtime/runtime.example.json). Schema version 1 rejects
unknown fields and invalid types. `validate` parses and validates only; it does not create a database,
instantiate a model client, or perform network I/O.

`doctor` additionally resolves the credential reference and assembles the configured provider and
store. It does not call the model, but a SQLite configuration may open or create its database file.

Credentials cannot be embedded in the JSON document. `apiKeyEnv` names the environment variable that
Runtime Host resolves during startup, and `baseUrl` rejects URL user-info credentials. Diagnostic and
serialization APIs never include secret values. Schema validation remains independent of provider
assembly, so `doctor` or `start()` is the authoritative check for a built-in provider's required
credential. Providers without a default endpoint, such as `OpenAICompatible`, must also configure
`baseUrl`; `doctor` and `start()` enforce that provider-specific requirement.

```json
{
  "schemaVersion": 1,
  "llm": {
    "provider": "OpenAI",
    "model": "gpt-4.1-mini",
    "apiKeyEnv": "OPENAI_API_KEY"
  },
  "store": {
    "kind": "memory"
  }
}
```

`store.kind` may be `memory` or `sqlite`. SQLite requires a Wuwe build with `WUWE_HAS_SQLITE=1` and a
non-empty `store.path`.

## CLI

```text
wuwe-runtime version
wuwe-runtime validate --config runtime.json
wuwe-runtime doctor --config runtime.json
wuwe-runtime inspect-plugin --package plugins/com.example.tools
wuwe-runtime run --config runtime.json --prompt "Summarize this release."
wuwe-runtime serve --config runtime.json
```

When `--prompt` is omitted, `run` reads the prompt from standard input. Results, diagnostics, and
errors are emitted as JSON, except for the compact `version` output. Exit codes are stable:

| Code | Meaning |
| ---: | --- |
| 0 | Success |
| 2 | Invalid command line |
| 3 | Invalid configuration |
| 4 | Startup or dependency failure |
| 5 | Agent execution failure |

An invalid package passed to `inspect-plugin` uses exit code 3 and emits a stable
`plugin_package_error_code` string in the JSON `error` field.

`serve` uses newline-delimited JSON on standard input and output. Each line is one Wuwe Agent Host
Protocol envelope. The protocol version and operations are documented in
[Agent Host Protocol](agent-host-protocol.md). Mutating requests require an idempotency key. Replays
return the original submission; within an operation, using the same key with different call metadata
or input returns `conflict`. Runtime Host stores a bounded digest rather than the original request in
its replay repository, and rejects sensitive names in call, run, and approval metadata.
Retryable availability failures are shared with concurrent duplicates but are not retained for later
replays, so the same key can be retried after capacity or availability recovers. Revision conflicts
remain replay-stable.

With the default SQLite run store, completed mutation outcomes and fingerprints are persisted in the
same database and replay across host restarts while retained by the bounded repository. The memory
store and custom run-store factories use a process-local replay repository unless the product
provides stronger coordination outside Runtime Host.

The run mutation and replay completion record use separate transactions. If the process terminates in
that narrow interval, a later request receives retryable `unavailable` with an unresolved-outcome
message instead of risking duplicate execution. Clients then reconcile using the caller-assigned run
ID and durable run state. Runtime Host therefore provides safe cross-restart replay of completed
outcomes without claiming atomic exactly-once execution across a process crash.

## Embedding API

```cpp
#include <wuwe/runtime_host.hpp>

auto config = wuwe::runtime_host::load_runtime_config("runtime.json");

wuwe::runtime_host::host host;
host.configure(std::move(config));
host.start();

auto result = host.run("Explain the deployment status.");
host.stop();
```

`runtime_dependencies` provides explicit model-client and run-store factories for products that need
dependency injection. It also exposes an agent-runner factory so products can configure the real
Wuwe runner without replacing Runtime Host orchestration. Custom model-client factories are not
coupled to the built-in provider registry. The default factories use Wuwe's registered providers and
in-memory/SQLite stores. The public lifecycle is:

```text
created -> configured -> starting -> ready -> draining -> stopped
                                      \--------------------> failed
```

The host stops accepting work when draining, propagates cancellation to active model calls, and waits
up to `shutdownTimeoutMs` before reporting a failed shutdown.
For embedded calls, `host::run()` combines the execution context's `stop_token` with Runtime Host
cancellation, and rejects reused run identifiers with the stable `run_conflict` error code.
Host Protocol submissions wait up to `submissionTimeoutMs` for their initial durable record before
returning a retryable availability error.

## Plugin packages

Runtime Host admits a plugin directory through `load_plugin_package()`. A package uses the fixed
`wuwe-plugin.json` manifest name and must contain a regular process entrypoint beneath its canonical
package root. Admission rejects missing files, directory entrypoints, symbolic links, reparse points,
hard-linked files, traversal, non-portable entrypoint paths, oversized manifests, and files that
change size or modification time while they are inspected. The resulting immutable `plugin_package`
exposes canonical paths and a SHA-256 content identity derived from the manifest and entrypoint
bytes.

`plugin_catalog` publishes immutable snapshots for concurrent readers. It keeps one active package
per plugin ID, treats identical content as an idempotent registration, rejects different content by
default, and requires an explicit replacement policy to update an existing ID. Snapshots support
deterministic lookup by ID and declared capability:

```cpp
auto package = wuwe::runtime_host::load_plugin_package("plugins/com.example.tools");

wuwe::runtime_host::plugin_catalog plugins;
auto registered = plugins.register_package(package);
auto tool_plugins = plugins.snapshot().providing("tools");
```

Package admission establishes filesystem identity for catalog and policy decisions. The built-in
process supervisor reloads and rehashes the package immediately before launch. This detects changes
between catalog admission and the launch check; it is not a claim that a trusted host filesystem is
immune to every time-of-check/time-of-use race.

## Plugin execution boundary

Runtime API version 1 supports process entrypoints and uses MCP over standard input/output as the
tool protocol. It intentionally does not load arbitrary native DLLs or shared libraries, avoiding a
cross-compiler C++ ABI contract.

`plugin_process_supervisor` separates package admission from execution and exposes start, stop,
unload, snapshot, tool discovery, and tool invocation operations. The default
`sandboxed_process` mode fails closed with `isolation_unavailable`: the built-in launcher does not
yet provide persistent interactive filesystem and network sandbox enforcement. A product may opt in
to `trusted_process` only for code it already trusts:

```cpp
auto package = wuwe::runtime_host::load_plugin_package("plugins/com.example.tools");

wuwe::runtime_host::plugin_process_supervisor processes({
  .isolation = wuwe::runtime_host::plugin_process_isolation::trusted_process,
  .environment = { { "PLUGIN_API_TOKEN", token } },
});

auto process = processes.start(package);
auto tools = processes.list_tools(package->manifest().id);
auto result = processes.call_tool(
  package->manifest().id, "search", { { "query", "Wuwe" } });
processes.unload(package->manifest().id);
```

Products that want coordinated package and process lifecycle can use the higher-level
`plugin_runtime` façade. It keeps the catalog and supervisor as separate internal modules while
making cross-module transitions explicit:

```cpp
wuwe::runtime_host::plugin_runtime plugins({
  .process_options = {
    .isolation = wuwe::runtime_host::plugin_process_isolation::trusted_process,
    .environment = { { "PLUGIN_API_TOKEN", token } },
  },
});

plugins.register_package("plugins/com.example.tools");
plugins.activate("com.example.tools");
auto result = plugins.call_tool(
  "com.example.tools", "search", { { "query", "Wuwe" } });
plugins.deactivate("com.example.tools");
```

Activation is idempotent while a process is running. A rejected catalog conflict leaves the active
process untouched. Explicit `plugin_registration_policy::replace` deactivates older content before
publishing the replacement, and `remove_package()` deactivates before unregistering. Deactivation
preserves the package registration so it can be activated again.

Plugin tools are composed into an agent only through explicit bindings. The plugin must be active
when `plugin_tool_provider` is constructed so the provider can discover and retain its MCP schema.
An empty `exposed_name` becomes `<plugin-id>__<tool-name>` by default; products can set a stable
public name instead:

```cpp
plugins.activate("com.example.tools");

auto plugin_tools = std::make_shared<wuwe::runtime_host::plugin_tool_provider>(plugins,
  std::vector<wuwe::runtime_host::plugin_tool_binding> {
    {
      .plugin_id = "com.example.tools",
      .tool_name = "search",
      .exposed_name = "search_knowledge",
    },
  });

wuwe::llm_agent_runner runner(client, plugin_tools);
auto response = runner.complete("Find the deployment guide.");
```

The provider never imports every advertised plugin tool implicitly. Each binding becomes a normal
Wuwe Tool Contract descriptor with the admitted package version and SHA-256 identity, policy
approval, an unknown idempotency classification, a write side-effect classification, and a
high-risk `runtime.plugin.tool.call` capability scoped to `<plugin-id>:<tool-name>`. Products can
therefore apply the same approval and capability policy used for other Wuwe tools, or compose the
provider with other providers through `compose_tool_providers()`.

Descriptors are a binding-time snapshot pinned to the admitted package content. If a plugin is later
deactivated, invocation returns an `unavailable` Tool outcome; it is not silently reactivated.
Reactivating the same package preserves the binding, while replacing that plugin ID with different
content requires constructing a new provider before calls are accepted. Cancellation is honored
before the synchronous MCP dispatch boundary. The current protocol path does not claim cooperative
in-flight cancellation, heartbeat propagation, or exactly-once tool execution. `host` still does
not inject plugin tools into agent runs automatically; the owning product chooses the bindings and
runner composition explicitly.

Trusted launch does not inherit the parent environment. Runtime supplies only its three reserved
metadata variables plus environment names both declared by the manifest and explicitly granted in
`plugin_process_options`. Missing grants fail before process creation. Child processes are grouped
with the plugin and terminated on unload or supervisor destruction (Windows Job Objects; POSIX
process groups).

Manifest `readPaths`, `writePaths`, and `networkHosts` remain policy declarations in trusted mode;
the built-in trusted launcher does not enforce them and snapshots report
`permissions_enforced == false`. Capability declarations are enforced at the Runtime API boundary:
for example, `list_tools()` and `call_tool()` require the `tools` capability. This is process
supervision and protocol gating, not a security sandbox.
