# Changelog

All notable changes to Wuwe are documented in this file. Wuwe follows
[Semantic Versioning](https://semver.org/) for its documented public C++ source
API. Cross-release C++ ABI compatibility is not promised; rebuild consumers
when upgrading the SDK.

## [1.0.0] - Unreleased

Wuwe 1.0.0 is the first stable public release. It replaces the unreleased
0.2.0 development identity and establishes the 1.x compatibility contract.

### Added

- A first-class Skills module with strict versioned manifests, immutable verified
  packages, SemVer dependency resolution, bounded activation, trust-aware LLM and
  Planning projection, explicit Knowledge/Multi-Agent/A2A/MCP adapters, and
  independent context-budget accounting.
- Durable agent runs with persistence, recovery, approval, cancellation,
  scheduling, lifecycle events, and host integration.
- Hard byte and estimated-token ceilings for model-visible Tool output, with
  UTF-8-safe previews, structured error preservation, per-tool tightening,
  typed preflight/failure results, lightweight estimator and projection headers,
  observability, Memory isolation, atomic Context Budget handling for Tool
  exchanges, digest-only durable projection audits, and durable continuation policy.
- Resource-aware LLM routing with capability checks, token budgets, cost
  budgets, usage accounting, and provider dispatch.
- Planning, reflection, guarded Best-of-N reasoning, typed orchestration,
  bounded fan-out/fan-in, and controlled retries.
- Local multi-agent teams, consensus, planning adapters, A2A discovery,
  messages, tasks, artifacts, JSON-RPC, and HTTP transport.
- Guardrail pipelines, weighted evaluation suites, trajectory regression,
  security evaluators, audit contracts, approvals, and observability.
- Scoped memory, file and SQLite persistence, embeddings, knowledge loading,
  retrieval, reranking, grounding, citations, and optional Qdrant adapters.
- MCP server, client, subprocess host, gateway, stdio and HTTP transports,
  protocol negotiation, access policy, audit, and telemetry.
- Offline learning/adaptation and controlled exploration workflows with
  explicit review, activation, rollback, and evidence boundaries.
- Root-scoped filesystem tools, allowlisted process tools, controlled Python
  execution, opt-in native restricted-process backends on Windows and macOS, and a versioned
  platform-neutral sandbox policy/plan compilation API with fail-closed
  enforcement reporting. The Windows backend consumes private versioned plans,
  enforces policy resource caps and filesystem precedence, validates runtime and
  filesystem capabilities, rejects reparse/hard-link escapes, and restores
  temporary AppContainer ACL leases after execution. The macOS backend compiles
  private Seatbelt plans, binds its Python framework runtime, denies network access,
  and applies process-group lifecycle and CPU safeguards. The compatibility factory
  uses the same native compiler and launch path on each supported host.
- Installable Windows, Linux, and macOS SDK packages, generated version headers,
  package manifests, bundled Tika/Temurin runtime options, and CMake consumer
  smoke tests.

### Changed

- The minimum CMake version is 3.20; the included presets require CMake 3.25 or
  newer.
- Wuwe now requires C++20 through its exported target without changing a
  source consumer's global C++ standard.
- CMake package compatibility is major-version scoped for 1.x source
  compatibility after recompilation.
- Public execution, storage, protocol, policy, and observability contracts are
  explicitly versioned or capability-described.

### Compatibility

- Applications upgrading from v0.1.0 must perform a clean rebuild.
- Do not combine v0.1.0 libraries with 1.0.0 headers, or the reverse.
- Container and WebAssembly execution backends are contract placeholders and
  are not implemented in 1.0.0.
- macOS 14+ on Apple Silicon is part of the 1.0.0 certification matrix. Intel
  macOS is outside the certified release profile.

See [docs/migration-1.0.md](docs/migration-1.0.md) for the upgrade procedure and
[docs/versioning.md](docs/versioning.md) for the compatibility contract.
