---
id: intro
title: Wuwe overview
slug: /
sidebar_position: 1
description: A concise map of Wuwe 1.0.0 and its supported deployment boundary.
---

# Wuwe overview

Wuwe is a C++20 framework for building tool-using, stateful, and auditable AI agents in native applications, services, and command-line programs.

Root-scoped filesystem tools and allowlisted process execution provide reusable
local operations without making an unrestricted shell the default integration
surface.

Its modules are independently usable. A host can start with one provider client and typed tools, then add reasoning, planning, memory, retrieval, MCP, or controlled execution only where needed.

## Modules

| Module | Responsibility |
| --- | --- |
| LLM providers | Provider configuration, normalized requests and responses, streaming, retries, and errors |
| Context budget | Unified allocation across system prompts, conversation, memory, retrieval, tool schemas/results, and reserved output |
| Resource routing | Capability-aware model profiles, token and cost budgets, and dynamic model selection |
| Tools | Typed schemas, JSON argument parsing, dispatch, and provider composition |
| Skills | Strict packages, SemVer dependencies, immutable registry snapshots, governed activation, and explicit runtime projections |
| Filesystem | Root-scoped reads, writes, exact edits, search, transfer, revisions, approvals, and audit |
| Process | Allowlisted argv execution, bounded I/O, process-tree cleanup, and an opt-in shell adapter |
| Reasoning | Simple, ReAct, reflect-and-retry, plan-execute, and Best-of-N runs with budgets and traces |
| Reflection | Rule-based or model-based evaluation, revision guidance, policy, and persistence |
| Planning | Plan generation, validation, dependency execution, retries, replanning, approvals, and checkpoints |
| Multi-Agent | Agent registry, roles and skills, lifecycle, capacity, shared sessions, parallel work, consensus, and Planning dispatch |
| A2A | Agent Card discovery, remote Messages, Tasks, Artifacts, JSON-RPC/HTTP transport, and local/remote Agent adapters |
| Guardrails | Ordered input/output and runtime-boundary checks with modification, denial, approval, audit, and telemetry |
| Evaluation | Weighted output, structured-result, and trajectory regression suites |
| Learning and adaptation | Experience and reward ledgers, versioned artifacts, offline optimization, regression gates, approval, activation, and rollback |
| SFT and adapter training | Independent governed module, typed Agent datasets, Full/LoRA/QLoRA variants, typed errors, checkpoints, strict providers, and Learning handoff |
| Exploration and discovery | Bounded hypotheses, approved experiments, evidence review, confidence thresholds, persistence, and explicit evidence export |
| Orchestration | Typed flows with branching, bounded fan-out/fan-in, filtering, retry, cancellation, recovery, and routing primitives |
| Memory | Scoped records, context injection, persistence, ranking, embeddings, and model-visible tools |
| Knowledge / RAG | Loading, splitting, indexing, retrieval, reranking, grounding, and citation support |
| MCP | Server, client, host, gateway, stdio, process, and HTTP integration |
| Networking | A common HTTP interface with cpr/libcurl and cpp-httplib backends |
| Capability policy | Explicit authorization decisions for sensitive actions |
| Approvals | Host-controlled approval requests and decisions |
| Audit | Structured event sinks for security-relevant operations |
| Controlled execution | Policy-bound Python subprocess execution with limits and cancellation |
| Sandbox contracts | Isolation and enforcement capability descriptions exposed by execution backends |
| Observability | Common events, module observers, traces, metrics adapters, and host-owned sinks |
| Agent Host protocol | Versioned, transport-neutral run, approval, resume, cancellation, and event contracts |
| Storage contracts | Explicit durability, transaction, migration, replay, and coordination guarantees |

## Release boundary

Version 1.0.0 is verified on Windows x64 with Visual Studio 2022, Ubuntu 24.04 Linux x64, and
macOS Apple Silicon with AppleClang.

The release is an SDK, not a hosted agent product. Applications retain ownership of user identity, secrets, UI, storage policy, approvals, and deployment topology.

Optional capabilities stay explicit:

- Windows uses Schannel by default; Linux and macOS release builds use OpenSSL.
- SQLite is required by the official release presets but remains configurable for custom builds.
- Default packages bundle Tika and a platform-specific Java 21 runtime for document parsing; either runtime component can be omitted for core-only or host-managed deployments.
- Qdrant and other remote indexes are external services configured by the host.
- `controlled_process` applies policy and resource limits but is not a strong isolation boundary.

## Start here

1. [Build and run Wuwe](getting-started.md).
2. Configure an [LLM provider](llm-providers.md) and [typed tools](llm-tools.md).
3. Compose the [agent runtime](agent-runtime.md), reusable [Skills](skills.md), optional [Agent Host protocol](agent-host-protocol.md), [orchestration](orchestration.md), [reasoning](reasoning.md), [planning](planning.md), [multi-agent runtime](multi-agent.md), [reflection](reflection.md), [learning and adaptation](learning-adaptation.md), [SFT and adapter training](training.md), or [exploration and discovery](exploration-discovery.md) layer you need.
4. Add [memory](memory-management.md), [knowledge retrieval](knowledge-retrieval.md), [MCP](mcp.md), or remote [A2A](a2a.md) interoperability.
5. Review [security and governance](security-governance.md), [observability](observability.md), [storage contracts](storage-contracts.md), [dependencies](dependencies.md), [packaging](packaging.md), and [controlled execution](execution-runtime.md) before deployment.
