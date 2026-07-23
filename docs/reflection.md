---
id: reflection
title: Reflection
description: Evaluate outputs with rules or models and convert findings into explicit actions.
---

# Reflection

Reflection evaluates an existing candidate. It does not create a forward execution plan; it returns a score, issues, an action, and optionally a revised output.

## Reflectors

| Reflector | Use |
| --- | --- |
| `rule_reflector` | Deterministic checks for empty, short, invalid JSON, required, or forbidden content |
| `llm_reflector` | Rubric-based semantic evaluation and revision through an `llm_client` |
| `composite_reflector` | Merge results from multiple reflectors |

## Rule-based example

```cpp
namespace reflection = wuwe::agent::reflection;

auto reflector = std::make_shared<reflection::rule_reflector>(
  reflection::rule_reflector_options {
    .reject_empty_output = true,
    .min_output_chars = 40,
    .required_substrings = { "Planning", "Reflection" },
  });

reflection::reflection_runner runner({
  .reflector = reflector,
});

const auto run = runner.run({
  .task = "Check the conceptual distinction.",
  .original_input = "Compare Planning and Reflection.",
  .candidate_output = answer,
});
```

For semantic evaluation, construct `llm_reflector` with a client and model. Its rubric supports weighted criteria, per-criterion thresholds, evidence requirements, and optional revision.

## Policy and result

`reflection_policy` maps normalized findings to `pass`, `revise`, `retry`, `replan`, `block`, or `escalate`. Thresholds and critical/error behavior are explicit host configuration.

`reflection_result` contains the pass state, score, recommended action, structured issues, revised output, and metadata. `reflection_runner` records elapsed time, emits start and completion events, and can persist records through `in_memory_reflection_store` or `file_reflection_store`.

Planning can use `plan_reflection_gate` to review step results. Reasoning uses reflection in `reflect_and_retry` mode. In both cases, the surrounding module remains responsible for deciding whether and how to continue.

See `examples/src/reflection_example.cpp` for a model-based rubric evaluation.
