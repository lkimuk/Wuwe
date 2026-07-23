---
id: orchestration
title: Orchestration
description: Compose synchronous, strongly typed agent flows with explicit branching, retry, and recovery.
---

# Orchestration

The orchestration module composes model calls and ordinary C++ transformations into an in-process, strongly typed flow. Each step remains visible in code, and the final flow runs synchronously through `invoke()`.

## Build a flow

Include `<wuwe/agent/orchestration/flow_primitives.hpp>` for the flow primitives and `<wuwe/net/net_errc.h>` for the retry predicate below.

```cpp
auto to_prompt = [](const std::string& input) {
  return "Extract the technical details from:\n\n" + input;
};

auto pipeline =
  client
  | to_prompt
  | wuwe::tee([](const wuwe::llm_response& response) {
      wuwe::println("first pass: {}", response.content);
    })
  | wuwe::filter(
      [](const wuwe::llm_response& response) {
        return response && !response.content.empty();
      },
      "model returned no content")
  | wuwe::retry_if(
      [](const wuwe::llm_response& response) {
        return response.error_code == wuwe::net_errc::rate_limited;
      },
      1)
  | wuwe::recover([](const std::exception& error) {
      return wuwe::llm_response {
        .content = std::string("flow failed: ") + error.what(),
      };
    });

const auto response = pipeline.invoke(input);
```

When a step returns a string, string view, C string, or `llm_request`, the flow sends it to the bound `llm_client`. Other values continue to the next step unchanged. This allows prompt construction, model calls, validation, routing, and result conversion to share one typed pipeline.

## Flow primitives

| Primitive | Responsibility |
| --- | --- |
| `identity` | Pass a value through unchanged |
| `tee` | Run a side effect such as logging while preserving the value |
| `apply_if` | Apply a transformation only when a predicate matches |
| `filter` | Reject a value by throwing `filter_error` |
| `if_else` | Select one of two transformations |
| `when`, `otherwise`, `route` | Route a value through the first matching branch |
| `retry_if` | Re-run the composed upstream flow when its result matches a predicate |
| `recover` | Convert an exception from the upstream flow into a fallback value |

Branches must produce compatible return types because the flow is checked by the C++ type system. `retry_if` also requires the original input to be copy constructible.

## Boundary

Orchestration is a lightweight composition layer, not a persistent workflow engine, task queue, or distributed scheduler. Use [Planning](planning.md) when work needs dependency graphs, checkpoints, replanning, approvals, or resumable step execution.

See `examples/src/chain_example.cpp`, `flow_example.cpp`, and `routing_example.cpp` for built examples.
