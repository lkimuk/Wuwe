# Wuwe v1.0.0

Wuwe 1.0.0 is the first stable public release of the C++20 framework for building
tool-using, stateful, and auditable AI agents. It establishes the Wuwe 1.x public
source-compatibility contract and expands the framework substantially beyond
v0.1.0.

## Highlights

### Production agent runtime

- Durable runs with persistence, recovery, cancellation, approvals, scheduling,
  lifecycle events, continuation policy, and host integration.
- Versioned Agent Host JSON envelopes for exposing Wuwe through application-owned
  HTTP, IPC, named-pipe, or sidecar transports.
- Explicit execution, storage, protocol, policy, audit, and observability contracts.

### Skills and tools

- Strict, versioned Skill manifests and immutable verified packages.
- SemVer dependency resolution, bounded activation, trust-aware instruction
  projection, and Knowledge, Planning, Multi-Agent, A2A, and MCP adapters.
- Typed Tool contracts, governed filesystem and process tools, and hard byte and
  estimated-token limits for model-visible Tool output.

### Models, reasoning, and orchestration

- OpenAI-compatible, Anthropic, Gemini, Ollama, and OpenRouter integrations.
- Capability-aware provider dispatch with token budgets, cost budgets, usage
  accounting, retries, and circuit breaking.
- Planning, reflection, guarded Best-of-N reasoning, typed chains, bounded
  fan-out/fan-in, dynamic parallel mapping, and resource-aware routing.

### Multi-agent interoperability

- Local agent teams, consensus, team planning, cancellation, and lifecycle control.
- A2A Agent Cards, Messages, Tasks, Artifacts, discovery, JSON-RPC, and HTTP
  transport.
- MCP servers and clients, subprocess hosting, gateways, stdio and HTTP transports,
  access policy, audit, and telemetry.

### State, knowledge, and governance

- Scoped memory with file and SQLite persistence, embeddings, vector and lexical
  retrieval, reranking, grounding, and citations.
- Knowledge loaders, optional Qdrant adapters, and bundled Apache Tika and Temurin
  runtime support.
- Guardrail pipelines, weighted evaluation suites, trajectory regression, security
  evaluators, approvals, auditing, and observability.
- Offline learning and adaptation plus controlled exploration, evidence review,
  activation, rollback, and regression gates.

### Controlled execution

- Cross-platform `controlled_process` execution with interpreter probing, policy,
  timeouts, cancellation, process cleanup, and bounded output.
- Native opt-in `restricted_process` isolation using AppContainer on Windows and a
  deny-default Seatbelt profile on macOS.
- Versioned sandbox policies and native plans with explicit capability reporting and
  fail-closed handling of unsupported enforcement requirements.

## Supported platforms

| Platform | Release package | Certified profile |
| --- | --- | --- |
| Windows x64 / Visual Studio 2022 | `wuwe-1.0.0-windows-x64.zip` | cpr and cpp-httplib builds |
| Ubuntu 24.04 Linux x64 | `wuwe-1.0.0-linux-x64.tar.gz` | OpenSSL and SQLite Release build |
| macOS 14+ Apple Silicon | `wuwe-1.0.0-macos-arm64.tar.gz` | AppleClang Release build |

The installed SDK contains public headers, static libraries, CMake package files,
documentation, examples, a package manifest, checksums, and optional Tika/Temurin
runtime sidecars.

## CMake consumption

Wuwe now exports a versioned CMake package and the `wuwe::wuwe` target:

```cmake
find_package(wuwe 1 CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE wuwe::wuwe)
```

Wuwe publishes its C++20 requirement and enabled public link dependencies through
the imported target. SQLite and OpenSSL remain consumer-visible dependencies when
the selected package profile enables them.

## Upgrade notes for v0.1.0 users

This is a major upgrade. Consumers must:

1. Remove old build directories and cached Wuwe package paths.
2. Install a complete 1.0.0 SDK from one package.
3. Reconfigure and rebuild every binary linked against Wuwe.
4. Run application-level integration, persistence, security, and migration tests.

Do not mix v0.1.0 libraries with 1.0.0 headers, or the reverse. Wuwe 1.x promises
documented public C++ source compatibility after recompilation; cross-release C++
ABI compatibility is not promised.

Back up production data before the first 1.0.0 deployment and exercise SQLite
migration and rollback with representative data.

## Cross-language integration

Wuwe 1.0.0 is a native C++ SDK. Non-C++ applications can integrate through the
versioned Agent Host JSON protocol, MCP stdio or HTTP, and A2A JSON-RPC/HTTP.
Official C ABI, Python, JNI, .NET, Rust, and Go language bindings are not included
in this release.

## Known limitations

- The stronger `restricted_process` backend is available on Windows and macOS;
  Linux reports it as unavailable instead of weakening the requested policy.
- The macOS restricted backend does not claim a complete virtual-memory ceiling or
  per-tree process-count limit. Policies requiring unsupported controls fail closed.
- Container and WebAssembly execution backends are public contract placeholders and
  are not implemented in 1.0.0.
- Filtered outbound sandbox networking is not implemented; unsupported network
  policies are rejected before launch.
- The bundled macOS JRE is ad-hoc signed during package creation. Official public
  distribution still requires release-channel signing and notarization.
- File-based stores do not claim transactions, multi-process coordination, or
  automatic schema migration.

## Verification

The release candidate has passed:

- Windows Release builds with both cpr and cpp-httplib HTTP backends;
- Ubuntu 24.04 Release build, tests, install, consumer, and package inspection;
- macOS 15 and macOS 26 Apple Silicon builds and sandbox contract tests;
- macOS install, bundled runtime, consumer, and package inspection;
- AddressSanitizer and ThreadSanitizer concurrency suites;
- source-subdirectory and installed-package consumer tests;
- release metadata, formatting, and documentation production builds.

## Documentation

- [Getting started](docs/getting-started.md)
- [Migrating to Wuwe 1.0](docs/migration-1.0.md)
- [Versioning and compatibility](docs/versioning.md)
- [Packaging](docs/packaging.md)
- [Sandbox architecture](docs/sandbox-architecture.md)
- [Changelog](CHANGELOG.md)
