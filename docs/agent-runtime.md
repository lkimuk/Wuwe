# Agent Runtime

Wuwe's agent runtime is host-application neutral. It does not know about UI frameworks,
document models, tabs, databases, or application sessions. Host applications provide those
through tool providers and keep ownership of their own state.

## Synchronous Runner

`llm_agent_runner::complete()` remains the simplest API:

```cpp
wuwe::llm_agent_runner runner(client, provider);
auto response = runner.complete("Analyze the current document.");
```

For cancellable or observable runs, pass `llm_agent_run_options`:

```cpp
std::stop_source stop_source;

wuwe::llm_agent_run_options options;
options.stop_token = stop_source.get_token();
options.callbacks.on_tool_start = [](const wuwe::llm_tool_call& call) {
  // Update host UI or telemetry.
};
options.callbacks.on_tool_result =
  [](const wuwe::llm_tool_call& call, const wuwe::llm_tool_result& result) {
    // Observe tool completion.
  };
options.callbacks.on_done = [](const wuwe::llm_response& response) {
  // Consume the final answer.
};

auto response = runner.complete(request, std::move(options));
```

When the bound client supports true streaming and any streaming observer is set,
the runner uses the client's streaming path. `on_delta` receives legacy text
content deltas; `on_reasoning_delta` and `on_reasoning_done` receive
provider-supplied visible reasoning summaries when available; `on_stream_event`
receives the raw provider-normalized `llm_stream_event` values, including
`reasoning_delta` and `tool_call_delta`; and `on_event` receives Agent-level
lifecycle events such as `model_first_event`, `model_content_delta`,
`model_reasoning_delta`, `model_reasoning_completed`, `tool_call_building`,
`tool_call_ready`, `tool_started`, `tool_completed`, and `model_completed`.

`content_delta` is final answer text. Host applications should not render it as
thinking progress. When the provider does not supply reasoning summaries, use
real lifecycle states such as waiting for the model, preparing a tool call,
running a tool, or generating the final answer.

If the client does not support streaming, `on_delta` falls back to receiving the
available response content after each completed model call, and
`on_reasoning_done` receives any final `llm_response::reasoning_summary`.

OpenAI-compatible streaming is documented in [LLM Streaming](llm-streaming.md).

## Async Runner

`run_async()` starts a background `std::jthread` and returns an `llm_agent_run` handle:

```cpp
auto run = runner.run_async(request, std::move(options));

// From the host's cancellation path:
run.request_stop();

auto response = run.get();
```

The handle owns the worker. Destroying it requests stop and joins the worker through `std::jthread`.
Host applications should keep the handle somewhere with an explicit lifetime, such as the current
operation object for a document, tab, request, or background task.

The runner, its `llm_client`, any tool provider, and any memory/context objects referenced by the
runner must outlive the async run. Wuwe does not take ownership of host application state.

## Cancellation Contract

Cancellation is cooperative:

- `llm_client::complete(request, stop_token)` checks the token before and after the request.
- `openai_compatible_llm_client` also checks cancellation between retry attempts.
- `llm_agent_runner` checks cancellation before model calls, before tool calls, after tool calls,
  and before each follow-up model call.
- Tool providers may expose `invoke(name, arguments_json, stop_token)`. The runner will call that
  overload when it exists, otherwise it falls back to `invoke(name, arguments_json)`.

Cancelled runs return:

```cpp
response.error_code == wuwe::agent::llm_error_code::cancelled
```

When the model keeps requesting tools until `max_tool_rounds` is exhausted,
the runner returns a stable Wuwe error instead of a generic standard-library
resource error:

```cpp
response.error_code == wuwe::agent::llm_error_code::agent_loop_budget_exceeded
response.stop_reason == "tool_round_budget_exceeded"
```

The response metadata includes `used_tool_rounds`, `max_tool_rounds`,
`last_tool_call`, `last_tool_call_id`, `last_tool_arguments`,
`last_tool_result`, and `last_model_response`. Host applications should use
the error code or `stop_reason` for UI classification and treat the message as
a developer diagnostic.

OpenAI-compatible clients report missing credentials before network I/O when
`llm_client_config::require_api_key` is true. Set it to false for local compatible servers. If the
host must guarantee that no environment API key is attached, also set
`llm_client_config::load_api_key_from_environment` to false.

## Stateful Tools

State belongs to the host. A provider can expose stable reflected tools while keeping private
application state outside the model-visible JSON schema:

```cpp
class app_tool_provider {
public:
  std::vector<wuwe::llm_tool> tools() const;

  wuwe::llm_tool_result invoke(
    const std::string& name,
    const std::string& arguments_json,
    std::stop_token stop_token);

private:
  app_session& session_;
};
```

Use `wuwe::make_llm_tool<T>()`, `wuwe::parse_tool_arguments<T>()`, and
`wuwe::invoke_reflected_tool<T>(arguments_json, context)` to build providers without depending on
internal `detail` APIs.

When a host has multiple providers, compose them instead of writing a forwarding
provider:

```cpp
auto tools = wuwe::compose_tool_providers(app_tools, knowledge_tools, memory_tools);
wuwe::llm_agent_runner runner(client, tools);
```

Provider order is significant. The first provider wins duplicate tool names, and
`std::stop_token` is forwarded to child providers that support stop-aware
`invoke(...)`.
