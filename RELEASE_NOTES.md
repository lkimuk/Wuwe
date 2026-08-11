# Wuwe v1.0.0

## New features

- Added official macOS 14+ Apple Silicon support, including CMake presets,
  macOS 15/26 CI, SDK installation and packaging, a bundled Temurin runtime,
  and native `restricted_process` isolation through Seatbelt.
- Added cross-language integration through the versioned Agent Host JSON
  protocol, which can be exposed over HTTP, IPC, named pipes, or a sidecar,
  plus A2A JSON-RPC/HTTP interoperability for remote agents.
- Added the Skills system with strict manifests, verified immutable packages,
  SemVer dependency resolution, governed activation, and adapters for LLM,
  Planning, Knowledge, Multi-Agent, A2A, and MCP.
- Added durable agent runs with persistence, recovery, approvals, cancellation,
  scheduling, lifecycle events, continuation policy, and SQLite-backed storage.
- Added local multi-agent teams, agent registration, consensus, team planning,
  task lifecycle management, and local/remote agent adapters.
- Added guardrail pipelines, weighted evaluation suites, security evaluators,
  trajectory regression, and common approval, audit, and observability contracts.
- Added offline learning and adaptation, controlled exploration, evidence review,
  versioned artifacts, activation, rollback, and regression gates.
- Added capability-aware LLM dispatch, context and token budgets, usage and cost
  accounting, resilient retries, and bounded model-visible Tool output.
- Extended MCP from protocol version `2024-11-05` to `2025-06-18` while retaining
  compatibility, and added explicit protocol negotiation, Skill integration, and
  safer asynchronous task lifecycle handling.
- Extended Memory and Knowledge with execution-context identity scoping,
  provenance-aware prompt injection, access labels, safer concurrent retrieval,
  and explicit partial-failure reporting.
- Added governed filesystem and process runtimes plus versioned, portable sandbox
  policy and plan compilation with native Windows and macOS enforcement.
- Added guarded Best-of-N reasoning, prioritized planning, resource-aware routing,
  and bounded fan-out/fan-in orchestration.

## Fixes

- Fixed macOS Seatbelt execution failures caused by Xcode Python selection,
  framework runtime access, dyld loading, and AppleClang toolchain differences.
- Fixed Windows restricted-process staging and path-safety issues involving runner
  reparse points, external hard links, ACL restoration, and request path identity.
- Fixed detached Best-of-N state lifetime and asynchronous integration failures
  that could escape or outlive their owning runtime state.
- Fixed atomic shared-pointer and C++20 portability across MSVC, GCC, AppleClang,
  and their supported standard libraries.
- Fixed compiler-dependent JSON default handling and strengthened fail-closed
  sandbox validation, resource enforcement, and cleanup reporting.
