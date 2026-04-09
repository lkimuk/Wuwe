# Prompt Chaining Design

This document proposes a Prompt Chaining design for Wuwe.

The goal is to provide a workflow-oriented abstraction on top of the current LLM and tool system,
without breaking the existing `llm_client`, `llm_agent_runner`, and reflected tool APIs.

## Goals

Prompt Chaining should make it easy to:

- split a complex interaction into multiple explicit steps
- preserve structured intermediate state between steps
- combine LLM reasoning, tool execution, and deterministic control flow
- support retries, validation, and recovery
- keep the final API small and idiomatic for C++

## Non-goals

The first version should not try to be:

- a full agent framework with autonomous planning everywhere
- a graph orchestration system with arbitrary distributed execution
- a replacement for direct `client->complete(...)`
- a replacement for `client->build_tools<...>()` for simple single-turn tool use

Prompt Chaining should be a higher-level orchestration layer, not a new foundation.

## Design Principles

### 1. One step, one responsibility

Each chain step should have a narrow purpose.

Examples:

- analyze user request
- create a short plan
- call a tool
- draft an answer
- verify the draft

This keeps prompts short and failures easy to diagnose.

### 2. Structured state over long prose

Steps should communicate through structured state, not by repeatedly re-parsing long natural
language outputs.

This improves:

- determinism
- debuggability
- retry behavior
- validation

### 3. Explicit control flow

The runtime should know:

- which step runs first
- which step runs next
- when execution terminates
- when to retry
- when to fail

Avoid implicit behavior that is hard to inspect.

### 4. Keep the public API builder-friendly

The final user-facing API should feel like building a small workflow, not wiring a large framework.

### 5. Integrate with current primitives

Prompt Chaining should reuse:

- `llm_client`
- reflected tools
- existing error handling patterns
- JSON-based state and schema logic where useful

## Conceptual Model

A prompt chain is a named sequence of steps operating on a shared mutable state.

At runtime:

1. the user creates a chain
2. the chain is run with an initial input state
3. each step reads from state and writes back to state
4. the runtime decides the next step
5. the chain ends with either a final output or an error

## Core Abstractions

## `chain_state`

`chain_state` is the shared working memory of the chain.

Recommended shape:

```cpp
struct chain_state {
  std::string input;
  nlohmann::json values;
};
```

Rationale:

- `input` keeps the original user request easy to access
- `values` stores arbitrary structured intermediate artifacts
- JSON is already used in the LLM/tool layer, so it is a practical interchange format

Useful helper API:

```cpp
class chain_state {
public:
  std::string input;

  bool contains(std::string_view key) const;

  const nlohmann::json& get_json(std::string_view key) const;
  nlohmann::json& get_json(std::string_view key);

  template <typename T>
  T get(std::string_view key) const;

  template <typename T>
  void set(std::string_view key, T&& value);

  void erase(std::string_view key);

private:
  nlohmann::json values_;
};
```

This allows steps to exchange:

- structured LLM output
- tool arguments
- tool results
- verification reports
- control flags such as `verified`, `needs_tools`, `retry_count`

## `chain_step_result`

Every step should return a structured result describing what happened.

Recommended shape:

```cpp
struct chain_step_result {
  bool ok { true };
  bool done { false };
  std::optional<std::string> next_step;
  std::error_code error_code;
  std::string error_message;
};
```

Semantics:

- `ok == true`: step execution succeeded
- `done == true`: chain should finish now
- `next_step`: explicit next-step override
- `error_code` and `error_message`: structured failure information

This makes control flow easy to reason about.

## `prompt_chain_step`

All steps should implement a common interface.

```cpp
class prompt_chain_step {
public:
  virtual ~prompt_chain_step() = default;

  virtual std::string_view name() const = 0;
  virtual chain_step_result run(llm_client& client, chain_state& state) = 0;
};
```

This keeps the runtime simple and allows different step kinds to coexist.

## `prompt_chain`

The chain itself is a registry plus executor.

Recommended API:

```cpp
class prompt_chain {
public:
  prompt_chain& add_step(std::unique_ptr<prompt_chain_step> step);
  prompt_chain& set_start(std::string step_name);
  prompt_chain& set_max_steps(int value);

  chain_run_result run(llm_client& client, chain_state initial_state) const;
};
```

## `chain_run_result`

```cpp
struct chain_run_result {
  chain_state state;
  std::string output;
  std::error_code error_code;
  std::string error_message;

  explicit operator bool() const noexcept {
    return !error_code;
  }
};
```

This mirrors the style already used by `llm_response`.

## Step Types

The first version should only expose three built-in step kinds.

## Step Names

Each step has a `name`, for example:

```cpp
.llm("analyze", { ... })
.tool("lookup", { ... })
.decision("decide", ... )
```

The `name` is not just a display label. It is the stable identifier of the step inside the chain.

It is needed for:

- step registration inside `prompt_chain`
- selecting the chain start step
- explicit control-flow jumps through `next_step`
- decision-step routing
- diagnostics and future tracing

Without step names, the runtime would have to rely on implicit order only, which makes branching,
validation loops, and debugging much harder.

In other words, `name` is the node id of the workflow.

## 1. `llm_step`

An `llm_step` runs an LLM prompt using the current chain state.

Recommended config:

```cpp
struct llm_step_config {
  std::string name;
  std::string system_prompt;
  std::optional<std::string> output_schema_json;
  std::optional<std::string> state_key;
  std::optional<std::string> next_step;
};
```

Behavior:

- render the prompt using the current state
- call `client.complete(...)`
- if `output_schema_json` exists, request structured output
- store the output under `state_key` if provided
- optionally move to `next_step`

Good use cases:

- analyze request
- create plan
- draft answer
- verify answer

## 2. `tool_step`

A `tool_step` executes a deterministic tool operation.

This should support both:

- calling a reflected tool provider
- calling a C++ callback directly

Recommended callback-oriented config:

```cpp
struct tool_step_config {
  std::string name;
  std::function<llm_tool_result(chain_state&)> invoke;
  std::optional<std::string> result_key;
  std::optional<std::string> next_step;
};
```

Good use cases:

- call a reflected tool with prepared arguments
- transform data
- normalize previous LLM output
- fetch cached context

## 3. `decision_step`

A `decision_step` does not call the model. It decides what happens next based on current state.

Recommended config:

```cpp
struct decision_step_config {
  std::string name;
  std::function<chain_step_result(const chain_state&)> decide;
};
```

Good use cases:

- choose between tool path and no-tool path
- branch based on verification result
- stop when confidence is sufficient

## Execution Semantics

The runtime should execute the chain like this:

1. load initial state
2. jump to the configured start step
3. run the current step
4. if the step returns failure, stop with error
5. if `done == true`, stop successfully
6. if `next_step` is set, jump there
7. otherwise use the chain's default next-step ordering
8. stop with an error if maximum step count is exceeded

This provides predictable execution and prevents infinite loops.

## Retry and Recovery

Prompt Chaining becomes useful only when failures are manageable.

The runtime should support three recovery strategies.

### 1. Retry current step

Use for transient issues:

- provider rate limits
- malformed but fixable LLM output
- temporary tool failures

### 2. Fallback step

Use when one path is unavailable:

- tool unavailable
- schema-constrained generation fails repeatedly
- verification step fails too many times

### 3. Backtrack to an earlier step

Use when the plan itself was wrong:

- selected wrong tool
- created incomplete plan
- verification found a structural issue, not just a wording issue

The first version does not need all recovery features built into the core runtime, but the step
result shape should not prevent them later.

## State Conventions

To keep chains readable, adopt a few conventions.

Suggested state keys:

- `analysis`
- `plan`
- `tool_args`
- `tool_result`
- `draft`
- `verification`
- `final_output`

Avoid deeply ad hoc key names across projects.

## LLM Output Strategy

Not every step needs free-form text.

Recommended split:

- reasoning and planning steps should usually emit structured JSON
- final-answer steps should emit user-facing text
- verification steps should emit structured pass/fail objects

Example verification object:

```json
{
  "passed": false,
  "issues": [
    "The answer does not mention the weather unit.",
    "The happy fact is missing."
  ]
}
```

This is much easier to consume than asking the next step to interpret long prose.

## Relationship With `llm_agent_runner`

`llm_agent_runner` should remain the simplest option for:

- single interaction
- one prompt
- optional tool loop

Prompt Chaining should become the right option when the workflow needs:

- multiple LLM calls
- branching
- validation
- explicit intermediate state

This separation is important. Do not collapse both abstractions into one type.

## Relationship With Reflected Tools

Prompt Chaining should be able to reuse reflected tools directly.

Example integration directions:

- a chain can create a tool-enabled sub-runner with `client.build_tools<Tools...>()`
- a tool step can call a reflected tool provider directly
- an LLM step may internally use a tool-enabled request when configured

Recommended first version:

- keep tool usage explicit in `tool_step`
- add LLM-with-tools substeps later if needed

This avoids combining too many concerns in the initial release.

## Builder API Proposal

The public API should feel compact.

One reasonable shape:

```cpp
auto chain = wuwe::prompt_chain_builder {}
  .llm("analyze", {
    .system_prompt = "Understand the user's request.",
    .state_key = "analysis"
  })
  .decision("decide", [](const wuwe::chain_state& state) {
    return state.get<bool>("needs_tools") ? "tool_call" : "draft";
  })
  .tool("tool_call", {
    .invoke = [](wuwe::chain_state& state) {
      return wuwe::llm_tool_result { .content = "..." };
    },
    .result_key = "tool_result",
    .next_step = "draft"
  })
  .llm("draft", {
    .system_prompt = "Write the final answer.",
    .state_key = "draft"
  })
  .build("analyze");
```

Execution:

```cpp
auto result = chain.run(*client, {
  .input = "What's the weather in Tokyo and give me one happy fact."
});
```

This style keeps usage concise while preserving explicit behavior.

## Minimal Example

Below is a realistic minimal chain:

```cpp
auto chain = wuwe::prompt_chain_builder {}
  .llm("analyze", {
    .system_prompt = "Extract the user goal and constraints as JSON.",
    .state_key = "analysis"
  })
  .tool("lookup", {
    .invoke = [](wuwe::chain_state& state) {
      return wuwe::llm_tool_result {
        .content = R"({"weather":"cloudy","temp_f":72})"
      };
    },
    .result_key = "weather",
    .next_step = "draft"
  })
  .llm("draft", {
    .system_prompt = "Write a final answer using analysis and weather.",
    .state_key = "final_output"
  })
  .build("analyze");

auto result = chain.run(*client, { .input = "What's the weather in Tokyo?" });
```

## Observability

Prompt Chaining will be much easier to debug if the runtime can expose execution traces.

Recommended future trace shape:

```cpp
struct chain_trace_event {
  std::string step_name;
  std::string kind;
  bool ok;
  std::string summary;
};
```

Even a lightweight trace vector in `chain_run_result` would help significantly.

## Recommended Scope for First Implementation

The first implementation should include only:

- `chain_state`
- `chain_step_result`
- `prompt_chain_step`
- `prompt_chain`
- `llm_step`
- `tool_step`
- `decision_step`
- a small builder
- a step-count safety limit

The first implementation should defer:

- parallel branches
- nested chains
- automatic reflection-based state schemas
- built-in persistence
- advanced scheduling
- UI/trace visualization

## Open Questions

These decisions should be made during implementation:

1. Should `chain_state` expose typed getters/setters only, or also raw JSON mutation?
2. Should `llm_step` own prompt templating, or should callers provide a render callback?
3. Should `tool_step` be callback-first, reflected-tool-first, or support both equally?
4. Should retries live in the runtime core or in reusable wrapper steps?
5. Should final output be a dedicated field in state, or part of `chain_run_result` only?

## Summary

A good Prompt Chaining design for Wuwe should:

- model workflows as explicit named steps
- pass structured shared state between steps
- keep control flow readable
- reuse existing LLM and tool abstractions
- make validation and retry straightforward
- stay smaller and simpler than a full agent framework

The right mental model is not "many prompts glued together", but "a small deterministic workflow
that happens to use prompts as one kind of step".
