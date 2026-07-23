---
id: agent-runtime
title: Agent runtime
description: Run model and tool loops with callbacks, cancellation, and host-owned state.
---

# Agent runtime

`llm_agent_runner` coordinates model calls and typed tool calls. It remains host-application neutral: the application owns UI state, user identity, secrets, approvals, storage, and scheduling.

## Basic run

```cpp
struct get_weather {
  static constexpr std::string_view description =
    "Get the current weather for a city.";

  std::string city;

  std::string invoke() const {
    return city + " is 22C.";
  }
};

auto client = wuwe::make_llm_client("OpenAI", config);
auto runner = client->bind_tools<get_weather>();
const auto response = runner.complete("What's the weather in Tokyo?");
```

The runner sends tool schemas to the model, parses tool-call arguments, invokes the matching provider, appends the result, and continues until a final response or the configured tool-round limit is reached.

## Observation and cancellation

```cpp
std::stop_source stop_source;

wuwe::llm_agent_run_options options;
options.stop_token = stop_source.get_token();
options.callbacks.on_delta = [](std::string_view delta) {
  // Render final-answer text as it arrives.
};
options.callbacks.on_tool_start = [](const wuwe::llm_tool_call& call) {
  // Record or display the tool transition.
};
options.callbacks.on_tool_result =
  [](const wuwe::llm_tool_call&, const wuwe::llm_tool_result&) {
    // Observe the completed tool call.
  };

const auto response = runner.complete("Inspect the input.", std::move(options));
```

The callback surface includes normalized stream events, content deltas, provider-supplied reasoning summaries, tool lifecycle events, completion, errors, and cancellation. Wuwe does not invent reasoning text when a provider does not supply it.

`run_async()` returns an `llm_agent_run` with `request_stop()`, `wait()`, and `get()`. Cancellation is cooperative across the runner, HTTP transport, and tools that honor the supplied stop token.

## Memory

Constructors accept an optional `memory_context`. When present, the runner augments requests from scoped memory and records new conversation state through that context. The application still controls the memory policy and backing store.

## Policy boundary

`llm_agent_callbacks::allow_tool_call` can reject a proposed tool call before dispatch. Security-sensitive tools should also enforce their own capability, approval, path, and audit policies; a model-facing tool schema is not an authorization boundary.

Use [Reasoning](reasoning.md) when the run needs explicit modes, budgets, or traces. Use [Planning](planning.md) for dependency-aware multi-step execution.
