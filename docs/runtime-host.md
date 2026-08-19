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
- plugin manifest contracts.

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

`serve` uses newline-delimited JSON on standard input and output. Each line is one Wuwe Agent Host
Protocol envelope. The protocol version and operations are documented in
[Agent Host Protocol](agent-host-protocol.md). Mutating requests require an idempotency key. Replays
return the original submission; within an operation, using the same key with different call metadata
or input returns `conflict`. Runtime Host stores a bounded digest rather than the original request in
its process-local replay cache, and rejects sensitive names in call, run, and approval metadata.
Retryable availability failures are shared with concurrent duplicates but are not retained for later
replays, so the same key can be retried after capacity or availability recovers. Revision conflicts
remain replay-stable.

The version 1 replay cache is intentionally process-local. After a host restart, clients reconcile
`create_run` using the caller-assigned run ID and durable run state; Runtime Host does not claim the
cross-restart atomic idempotency guarantee required of a fully durable Host Protocol service.

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

## Plugin boundary

Version 1 defines a strict Plugin Manifest and supports only an isolated `process` entrypoint contract.
The manifest declares capabilities and permissions for environment access, paths, and network hosts.
Runtime Host does not yet launch plugin processes, and it intentionally does not load arbitrary native
DLLs or shared libraries. This preserves a stable product boundary without claiming a cross-compiler
C++ ABI.
