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
- Make cancellation cooperative through the HTTP, LLM client, runner, and tool
  invocation boundaries.

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

### OpenRouter / OpenAI-Compatible Client

`openrouter_llm_client::complete_stream()` sends the normal chat-completions
payload plus:

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
- `llm_agent_run_options.callbacks.on_delta` is set.

Otherwise the runner keeps the existing synchronous behavior.

The runner forwards content deltas to `on_delta`, waits for the final accumulated
response, invokes tools if the model requested them, and then streams the
follow-up model answer the same way.

## Cancellation

Cancellation is cooperative:

- `llm_agent_runner` checks before model calls, tool calls, and follow-up model
  calls.
- `openrouter_llm_client::complete_stream()` checks before attempts and during
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
- existing runner callback behavior for non-streaming clients.

Run:

```powershell
cmake --build build --config Debug --target agent_runtime_tests
ctest --test-dir build -C Debug -R agent_runtime_tests --output-on-failure
```
