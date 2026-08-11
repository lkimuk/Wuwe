# Wuwe v1.0.0

## New features

- Added a production agent runtime with durable runs, persistence, recovery,
  approvals, cancellation, scheduling, lifecycle events, and a versioned Agent
  Host protocol.
- Added governed Skills with strict manifests, verified packages, SemVer
  dependency resolution, bounded activation, and runtime adapters.
- Added bounded model-visible Tool output, governed filesystem and process tools,
  and controlled Python execution.
- Added capability-aware LLM routing, token and cost budgets, usage accounting,
  retries, and OpenAI-compatible, Anthropic, Gemini, Ollama, and OpenRouter support.
- Added planning, reflection, guarded Best-of-N reasoning, typed orchestration,
  bounded fan-out/fan-in, and resource-aware routing.
- Added local multi-agent teams plus A2A discovery, messaging, tasks, artifacts,
  JSON-RPC, and HTTP transport.
- Added MCP servers and clients, subprocess hosting, gateways, stdio and HTTP
  transports, access policy, audit, and telemetry.
- Added scoped memory, file and SQLite persistence, embeddings, retrieval,
  reranking, grounding, citations, optional Qdrant integration, and bundled
  Tika/Temurin runtime support.
- Added guardrails, evaluation suites, security evaluators, approvals, auditing,
  observability, offline adaptation, and controlled exploration workflows.
- Added cross-platform SDK packages and native restricted-process isolation using
  AppContainer on Windows and Seatbelt on macOS.

## Fixes

- Fixed macOS restricted execution so Seatbelt can launch the selected Python
  interpreter and read its framework runtime on supported Xcode toolchains.
- Fixed AppleClang C++20 portability and macOS CI toolchain compatibility.
- Fixed Windows restricted Python staging and CI behavior around runner reparse
  points.
- Fixed atomic shared-pointer portability across supported standard libraries.
- Fixed detached Best-of-N execution state lifetime and asynchronous failure
  containment.
- Fixed compiler-dependent JSON default handling and strengthened sandbox path,
  process, resource, and cleanup enforcement.
