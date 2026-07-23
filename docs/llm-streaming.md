---
id: llm-streaming
title: LLM streaming
description: Consume normalized content, reasoning, and tool-call events.
---

# LLM streaming

Wuwe exposes provider-normalized streaming at both the client and agent-runner levels.

## Direct client streaming

```cpp
wuwe::llm_request request;
request.model = config.model;
request.messages.push_back({
  .role = "user",
  .content = "Explain streaming in two sentences.",
});

wuwe::llm_stream_callbacks callbacks;
callbacks.on_event = [](const wuwe::llm_stream_event& event) {
  if (event.type == wuwe::llm_stream_event_type::content_delta) {
    std::cout << event.content_delta << std::flush;
  }
};

const auto response = client->complete_stream(request, callbacks);
```

Normalized event types cover content, provider-supplied reasoning summaries, tool-call deltas, completion, and errors. Tool-call name and argument fragments are assembled by the higher-level runner before dispatch.

## Runner streaming

```cpp
wuwe::llm_agent_run_options options;
options.callbacks.on_delta = [](std::string_view delta) {
  std::cout << delta << std::flush;
};
options.callbacks.on_tool_start = [](const wuwe::llm_tool_call& call) {
  std::cout << "tool: " << call.name << '\n';
};

const auto response = runner.complete("Use the available tools.", std::move(options));
```

Use `on_stream_event` for raw normalized stream events and `on_event` for agent lifecycle transitions such as model start, first event, tool-call assembly, tool execution, and completion.

`on_delta` contains final-answer text. `on_reasoning_delta` and `on_reasoning_done` contain only reasoning summaries supplied by the provider. Wuwe does not synthesize hidden chain-of-thought.

## Timeouts and cancellation

`llm_stream_timeout_options` separates:

- total request time;
- connection time;
- time to first event;
- maximum idle time between events.

Pass a `std::stop_token` through `llm_agent_run_options` for cooperative cancellation. `run_async()` also exposes `request_stop()`.

When a client does not offer native streaming, the runner can still report the completed response through its callback surface, but it cannot manufacture incremental transport events.

The complete executable example is `examples/src/llm_streaming_example.cpp`.
