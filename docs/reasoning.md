---
id: reasoning
title: Reasoning
description: Run explicit reasoning modes with budgets, traces, tools, reflection, and plans.
---

# Reasoning

`reasoning_runner` selects and executes a bounded strategy around model calls, tools, reflection, or planning. It reports a normalized result, resource usage, and a structured trace.

## Modes

| Mode | Behavior |
| --- | --- |
| `simple` | One model-oriented answer path without a tool loop |
| `react` | Model and tool loop through `llm_agent_runner` |
| `reflect_and_retry` | Generate, evaluate, and revise or retry within policy limits |
| `plan_execute` | Create and execute a dependency-aware plan |

## Tool-using run

```cpp
namespace reasoning = wuwe::agent::reasoning;

auto provider = std::make_shared<wuwe::tool_provider<get_weather>>();
auto runner = reasoning::reasoning_runner::with_tools(
  *client,
  provider,
  {
    .observer = [](const reasoning::reasoning_event& event) {
      if (event.type == reasoning::reasoning_event_type::content_delta) {
        std::cout << event.delta << std::flush;
      }
    },
  });

const auto result = runner.run({
  .input = "What's the weather in Tokyo?",
  .model = config.model,
  .policy = {
    .mode = reasoning::reasoning_mode::react,
    .budget = { .max_tool_rounds = 2 },
    .enable_streaming = true,
  },
});
```

## Budgets and results

`reasoning_budget` limits steps, model calls, tool calls, tool rounds, reflection attempts, and elapsed time. Exceeding a budget produces a typed reasoning error instead of silently continuing.

`reasoning_result` contains:

- completion state, content, and provider-supplied reasoning summary;
- final model response;
- optional plan and reflection runs;
- steps, trace records, and usage counts;
- typed and underlying errors;
- elapsed time.

The observer receives lifecycle, stream, tool, reflection, planning, completion, failure, and cancellation events. `run_async()` adds cooperative cancellation.

## Policy selection

`select_policy()` maps a `reasoning_task_profile` to a built-in policy. The available profiles cover simple answers, tool-required work, complex analysis, required planning, and high-confidence answers. Hosts can bypass selection and populate `reasoning_policy` directly.

`make_default_agentic_runner()` assembles the standard model/tool/reflection path. `make_knowledge_aware_runner()` adds the knowledge tool provider. These helpers compose existing public modules; they do not hide capability or storage policy.

See `examples/src/reasoning_example.cpp` for offline plan execution and a live ReAct run.
