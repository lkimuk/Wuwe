---
id: observability
title: Observability
description: Connect runtime events, traces, metrics, and module-specific telemetry to host-owned sinks.
---

# Observability

Wuwe exposes events and callbacks at runtime boundaries without prescribing a monitoring platform. Applications choose where telemetry is stored, how it is correlated, and which data is safe to retain.

## Common event sink

`agent_event` is the shared event envelope. It carries a module and event name, trace and subject identifiers, timestamp, elapsed time, and string attributes.

```cpp
namespace obs = wuwe::agent::observability;

auto memory_sink = std::make_shared<obs::in_memory_event_sink>();
auto file_sink = std::make_shared<obs::jsonl_event_sink>("agent-events.jsonl");
auto events = std::make_shared<obs::fanout_event_sink>();

events->add_sink(memory_sink);
events->add_sink(file_sink);

events->publish({
  .module = "application",
  .name = "agent_run_started",
  .trace_id = trace_id,
  .subject_id = user_id,
});
```

The built-in sinks support in-memory collection, JSON Lines output, and fan-out. Implement `event_sink` to connect Wuwe events to an existing logging, tracing, or telemetry stack.

## Module observation surfaces

Modules expose the event shape appropriate to their work:

| Module | Observation surface |
| --- | --- |
| Agent runtime | `llm_agent_callbacks` for stream, tool, completion, error, and cancellation events |
| Reasoning | `reasoning_observer`, normalized usage, and structured trace records |
| Planning | `plan_observer` and `plan_trace_sink` |
| Reflection | `reflection_observer` |
| Memory | A module-specific audit callback on `memory_context` |
| Knowledge / RAG | `knowledge_event_sink` with in-memory, common-event, Prometheus-text, and OTEL-style adapters |
| MCP host | `mcp_host_event_sink` with in-memory, JSONL, fan-out, common-event, Prometheus-text, and OTEL-style adapters |
| MCP server | A module-specific audit callback |
| Controlled execution | The common security `audit_sink` |

Knowledge and MCP host events can be bridged into the common sink with `agent_knowledge_event_sink` and `agent_mcp_host_event_sink`. Other module callbacks remain explicit so the host can normalize only the events it needs.

## Export boundary

The Prometheus sinks produce scrape text, and the OTEL-style sinks produce in-memory span representations. They do not automatically start an HTTP metrics endpoint or export to an OpenTelemetry collector.

Production integrations should define trace propagation, attribute naming, sampling, secret and prompt redaction, file rotation, retention, and failure behavior. Telemetry sinks run in the calling process, so blocking or throwing adapters can affect the operation that publishes an event.

Use [Security and governance](security-governance.md) for authorization and audit semantics. Module pages describe the domain-specific events emitted by each runtime.
