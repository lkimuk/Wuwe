# LLM Streaming

Wuwe supports OpenAI-compatible streaming as a first-class runtime path. The
implementation is intentionally split across small layers so token streaming,
tool-call streaming, cancellation, and retries stay testable and do not leak SSE
string handling into agent orchestration code.

## Design Goals

- Preserve the existing non-streaming `complete()` API and behavior.
- Expose true provider streaming through `complete_stream()`.
- Parse Server-Sent Events independently from provider-specific JSON.
- Emit structured stream events, not only raw text chunks.
- Accumulate streamed tool calls before the agent runner invokes tools.
- Keep runner callbacks stable: applications can use `on_delta` without knowing
  whether a client used true streaming or a fallback full response.
- Let host applications observe Agent progress without displaying raw tool
  argument JSON to users.
- Make cancellation cooperative through the HTTP, LLM client, runner, and tool
  invocation boundaries.
- Split streaming timeouts into transport and stream phases where the active
  HTTP backend can enforce them.

## Layering

### HTTP Transport

`http_client::send_stream()` accepts a chunk callback and a `std::stop_token`.
`default_http_client` delegates to the selected HTTP backend. The default build
uses cpr/libcurl; `-DWUWE_HTTP_BACKEND=httplib` switches the default to the
`cpp-httplib` backend. Both concrete backends implement the same streaming
contract and check cancellation while data is being received.

The transport layer does not understand SSE or LLM payloads. Its contract is
only:

- deliver body chunks in arrival order,
- stop when the callback returns false,
- return transport or HTTP errors,
- expose status codes and response headers for diagnostics,
- preserve the response body for diagnostics and fallback parsing.

### SSE Parser

`sse_event_parser` converts arbitrary byte chunks into complete SSE events. It
handles:

- events split across network chunks,
- multiple events in one network chunk,
- CRLF and LF line endings,
- comments,
- multi-line `data:` fields,
- final flush of a buffered event.

The parser does not parse JSON and does not know about OpenAI, OpenRouter, or
tool calls.

### LLM Stream API

`llm_client` now exposes:

```cpp
virtual bool supports_streaming() const noexcept;

virtual llm_response complete_stream(
  const llm_request& request,
  const llm_stream_callbacks& callbacks,
  std::stop_token stop_token = {});
```

The default implementation falls back to `complete()` and emits one synthetic
content event plus a done event. Providers that can stream override
`supports_streaming()` and `complete_stream()`.

Stream callbacks receive `llm_stream_event`:

- `content_delta`: text generated so far.
- `tool_call_delta`: partial tool id, name, or arguments.
- `tool_call_done`: one fully accumulated tool call.
- `done`: final accumulated `llm_response`.
- `error`: terminal failure, with partial response content when available.

### Provider Clients

All built-in LLM clients expose streaming through the same
`llm_client::complete_stream()` contract. Provider-specific clients translate
their native stream format into Wuwe's shared events:

- OpenAI-compatible presets parse SSE chat-completion deltas.
- Anthropic parses Messages API SSE events.
- Gemini parses `streamGenerateContent?alt=sse` events.
- Ollama parses line-delimited JSON chat chunks.

### OpenAI-Compatible Client And Provider Presets

`openai_compatible_llm_client::complete_stream()` sends the normal
chat-completions payload plus:

```json
{
  "stream": true,
  "stream_options": {
    "include_usage": true
  }
}
```

It parses OpenAI-compatible SSE chunks:

```text
data: {"choices":[{"delta":{"content":"Hel"}}]}

data: {"choices":[{"delta":{"content":"lo"},"finish_reason":"stop"}]}

data: [DONE]
```

For tool calls, OpenAI-compatible providers may stream `function.name` and
`function.arguments` in fragments. Wuwe accumulates fragments by tool-call
index, emits `tool_call_delta` events for observability, and returns complete
`llm_tool_call` values to the runner.

### Agent Runner

`llm_agent_runner` chooses true streaming when both conditions are met:

- `client.supports_streaming()` is true,
- at least one of `on_delta`, `on_stream_event`, or `on_event` is set.

Otherwise the runner keeps the existing synchronous behavior.

The runner forwards raw provider-normalized `llm_stream_event` values to
`on_stream_event` before applying legacy text handling. It also emits
Agent-native events through `llm_agent_callbacks::on_event`:

- `model_started`
- `model_first_event`
- `model_content_delta`
- `tool_call_building`
- `tool_call_ready`
- `tool_started`
- `tool_completed`
- `model_completed`
- `agent_completed`
- `agent_failed`
- `agent_cancelled`

Host UIs should use these events to show state such as "waiting for first model
event", "building a tool call", "running a tool", or "generating final answer".
They should not display streamed tool argument JSON directly unless the host has
made an explicit product decision to reveal it.

After the model call completes, the runner invokes tools if the model requested
them and then streams the follow-up model answer the same way.

### Reasoning Event Mapping

The Reasoning facade maps Agent streaming progress into reasoning events when
streaming is enabled:

- `model_first_event`
- `content_delta`
- `tool_call_building`
- `tool_call_ready`
- `model_started`
- `tool_started`
- `tool_completed`
- `model_completed`

This keeps ReAct and higher-level workflows observable without requiring hosts
to subscribe to both the Reasoning facade and the lower-level Agent runner.

## Streaming Timeouts

`llm_client_config::stream_timeouts` can split streaming budgets:

```cpp
wuwe::llm_client_config config {
  .timeout = 60000,
  .stream_timeouts = {
    .total_ms = 60000,
    .connect_ms = 5000,
    .first_event_ms = 15000,
    .idle_ms = 10000,
  },
};
```

For streaming requests, Wuwe maps these to HTTP timeout options:

- `total_ms`: whole request budget, falling back to `timeout` when unset.
- `connect_ms`: connection budget.
- `first_event_ms`: first stream event budget.
- `idle_ms`: maximum gap between stream events.

Built-in streaming clients also report parser-observed first-event and idle
timeouts as `llm_error_code::timeout` with response metadata:

```text
timeout_phase=first_event | idle
timeout_ms=<configured budget>
```

HTTP backends may surface a transport timeout before the provider parser sees an
event. In that case Wuwe still classifies the result as a timeout, but phase
metadata is only available when the LLM streaming layer observes the phase
boundary directly.

## Cancellation

Cancellation is cooperative:

- `llm_agent_runner` checks before model calls, tool calls, and follow-up model
  calls.
- `openai_compatible_llm_client::complete_stream()` checks before attempts and during
  chunk processing.
- `default_http_client::send_stream()` aborts the selected backend transfer if
  the stop token is requested or the callback returns false.

Cancelled calls return `llm_error_code::cancelled` when the LLM layer observes
the stop request.

## Retry Semantics

Non-streaming calls keep the existing retry policy.

Streaming calls retry only before any output has been emitted. Once a content or
tool-call delta has reached the caller, Wuwe does not transparently retry the
request because doing so could duplicate visible output or replay a partial tool
call.

## Error Semantics

If the provider returns a structured error before streaming begins, Wuwe maps it
through the existing LLM error classification.

If a stream fails after partial output, the final `llm_response` carries the
error code and the partial content collected so far. Callers should treat this
as a failed run with visible partial output.

Malformed SSE JSON is reported as `llm_error_code::invalid_response`.

## Usage

Direct provider streaming:

```cpp
wuwe::llm_stream_callbacks callbacks;
callbacks.on_event = [](const wuwe::llm_stream_event& event) {
  if (event.type == wuwe::llm_stream_event_type::content_delta) {
    wuwe::print("{}", event.content_delta);
  }
};

auto response = client->complete_stream(request, callbacks);
```

Runner streaming:

```cpp
wuwe::llm_agent_run_options options;
options.callbacks.on_delta = [](std::string_view delta) {
  wuwe::print("{}", delta);
};
options.callbacks.on_event = [](const wuwe::llm_agent_event& event) {
  if (event.type == wuwe::llm_agent_event_type::tool_call_building) {
    // Update host UI state without exposing raw tool arguments.
  }
};

auto response = runner.complete("Answer incrementally.", std::move(options));
```

Run the live example:

```powershell
$env:OPENROUTER_API_KEY = "your_api_key"
$env:OPENROUTER_CHAT_MODEL = "openai/gpt-oss-120b:free"
cmake --build build --config Debug --target llm_streaming_example
.\build\examples\Debug\llm_streaming_example.exe
```

## Tests

`agent_runtime_tests` covers:

- SSE events split across chunks,
- multiple SSE events batched into one chunk,
- content delta aggregation,
- streamed tool-call name and argument aggregation,
- `stream=true` request payload generation,
- raw stream-event forwarding through the Agent runner,
- Agent-native model/tool lifecycle events,
- first-event and idle timeout phase reporting,
- staged streaming timeout mapping,
- existing runner callback behavior for non-streaming clients.

Run:

```powershell
cmake --build build --config Debug --target agent_runtime_tests
ctest --test-dir build -C Debug -R agent_runtime_tests --output-on-failure
```
