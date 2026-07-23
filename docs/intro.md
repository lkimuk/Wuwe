---
id: intro
title: Wuwe overview
slug: /
sidebar_position: 1
description: A concise map of Wuwe 0.1.0 and its supported deployment boundary.
---

# Wuwe overview

Wuwe is a C++20 framework for building tool-using, stateful, and auditable AI agents in native applications, services, and command-line programs.

Its modules are independently usable. A host can start with one provider client and typed tools, then add reasoning, planning, memory, retrieval, MCP, or controlled execution only where needed.

## Modules

| Module | Responsibility |
| --- | --- |
| LLM providers | Provider configuration, normalized requests and responses, streaming, retries, and errors |
| Tools | Typed schemas, JSON argument parsing, dispatch, and provider composition |
| Reasoning | Simple, ReAct, reflect-and-retry, and plan-execute runs with budgets and traces |
| Reflection | Rule-based or model-based evaluation, revision guidance, policy, and persistence |
| Planning | Plan generation, validation, dependency execution, retries, replanning, approvals, and checkpoints |
| Orchestration | Typed flows with branching, filtering, retry, recovery, and routing primitives |
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

## Release boundary

Version 0.1.0 is verified on Windows x64 with Visual Studio 2022 and Ubuntu 24.04 Linux x64. macOS portability is an engineering goal, but macOS is not part of the 0.1.0 certification matrix.

The release is an SDK, not a hosted agent product. Applications retain ownership of user identity, secrets, UI, storage policy, approvals, and deployment topology.

Optional capabilities stay explicit:

- Windows uses Schannel by default; Linux release builds use OpenSSL.
- SQLite is required by the official release presets but remains configurable for custom builds.
- Tika and a platform-specific Java 21 runtime are bundled for default document parsing.
- Qdrant and other remote indexes are external services configured by the host.
- `controlled_process` applies policy and resource limits but is not a strong isolation boundary.

## Start here

1. [Build and run Wuwe](getting-started.md).
2. Configure an [LLM provider](llm-providers.md) and [typed tools](llm-tools.md).
3. Compose the [agent runtime](agent-runtime.md), [orchestration](orchestration.md), [reasoning](reasoning.md), [planning](planning.md), or [reflection](reflection.md) layer you need.
4. Add [memory](memory-management.md), [knowledge retrieval](knowledge-retrieval.md), or [MCP](mcp.md).
5. Review [security and governance](security-governance.md), [observability](observability.md), [dependencies](dependencies.md), [packaging](packaging.md), and [controlled execution](execution-runtime.md) before deployment.
