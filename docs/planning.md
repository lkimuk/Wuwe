---
id: planning
title: Planning
description: Build, validate, execute, persist, and resume dependency-aware plans.
---

# Planning

The planning module turns a goal into a validated graph of steps and executes ready steps under explicit retry, timeout, approval, and persistence policies.

## Planners and executors

| Component | Implementations |
| --- | --- |
| Planner | `static_planner`, `llm_planner` |
| Executor | `function_plan_executor`, `tool_plan_executor`, `agent_plan_executor`, `composite_plan_executor` |
| Store | `in_memory_plan_store`, `file_plan_store` |

`llm_planner` requests a JSON plan, repairs normalizable output, and validates tool and agent assignments. `static_planner` is useful for deterministic workflows and tests.

## Minimal plan

```cpp
namespace planning = wuwe::agent::planning;

auto planner = std::make_shared<planning::static_planner>(
  std::vector<planning::plan_step> {
    { .id = "inspect", .title = "Inspect input" },
    {
      .id = "summarize",
      .title = "Summarize result",
      .depends_on = { "inspect" },
    },
  });

auto executor = std::make_shared<planning::function_plan_executor>(
  [](const planning::plan_step& step,
     const planning::plan_execution_context&) {
    return planning::plan_step_result::completed(step.id + " completed");
  });

planning::plan_runner runner({
  .planner = planner,
  .executor = executor,
});

const auto result = runner.run({ .goal = "Inspect and summarize the input" });
```

## Execution semantics

`plan_runner` validates the graph, finds dependency-ready steps, executes up to `max_parallel_steps`, records outputs and artifacts, and persists checkpoints when a store is configured.

`plan_policy` controls maximum steps and iterations, attempts per step, steps per run, parallelism, step and run timeouts, replanning, failure continuation, and resume behavior. `resume()` resets interrupted running steps by default and continues from persisted state.

Policy hooks can allow, deny, or require approval for each ready step. An approval provider supplies the host decision. A reflection gate can review completed steps before the run proceeds.

## Validation and persistence

`plan_validator` checks identifiers, dependencies, cycles, limits, tool assignments, and agent assignments. `plan_normalizer` repairs safe structural issues. `plan_codec` serializes plans and results as JSON.

The file store is suitable for local checkpoints. Applications that need transactional shared storage can provide a custom `plan_store` backed by their database.

Planning events and trace callbacks expose plan creation, step transitions, approval requirements, revisions, cancellation, and completion.
