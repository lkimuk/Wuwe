---
id: llm-providers
title: LLM providers
description: Configure built-in cloud and local model clients through one interface.
---

# LLM providers

Wuwe normalizes provider configuration, requests, responses, streaming events, tool calls, usage, retries, and errors behind `llm_client`.

`llm_request::max_output_tokens` is the common output limit. Built-in OpenAI-compatible, Anthropic, Gemini, and Ollama clients translate it to their protocol-specific request field. `llm_usage` distinguishes prompt, completion, cached-prompt, and reasoning tokens. `calculate_llm_cost()` applies explicit per-million-token pricing and falls back to the normal input/output rate when a separate cache or reasoning rate is not supplied.

Common generation controls include `stop_sequences`, deterministic `seed`,
`json_schema_output`, and `cache_mode`. `llm_client::capabilities()` and the provider
registry declare which controls the configured adapter supports. Unsupported
controls return `llm_error_code::unsupported_capability` before network dispatch;
they are never silently dropped. Malformed schemas, empty stop strings, conflicting
legacy/structured response formats, and invalid output limits return
`llm_error_code::invalid_request`.

OpenAI, Gemini, and Ollama presets declare JSON Schema output and deterministic
seed support. Anthropic declares stop-sequence support. Generic
`OpenAICompatible` metadata is deliberately conservative; a custom endpoint can
set `llm_client_config::capabilities_override` only for features it actually
implements. Explicit cache enable/disable currently fails closed unless a custom
adapter declares and implements cache control.

The base `llm_client` reports an undeclared capability contract so existing custom
clients remain usable after recompilation. Wuwe still applies request-structure
validation, but feature rejection is enforced only when an adapter returns a
declared contract. Custom production clients should override `capabilities()`;
built-in and scripted clients always return declared contracts.

`llm_request::context_budget` optionally constrains the full request before each
agent model call. See [Context budget](context-budget.md). Direct low-level
`llm_client::complete()` calls do not rewrite the request; hosts using that boundary
can call `context_budget_manager::fit()` explicitly.

## Built-in providers

| Provider ID | Protocol | Default credential |
| --- | --- | --- |
| `OpenAI` | OpenAI-compatible chat completions | `OPENAI_API_KEY` |
| `OpenAICompatible` | Configurable OpenAI-compatible endpoint | `OPENAI_API_KEY` |
| `OpenRouter` | OpenAI-compatible | `OPENROUTER_API_KEY`, then `OPENAI_API_KEY` |
| `Anthropic` | Anthropic Messages | `ANTHROPIC_API_KEY` |
| `Gemini` | Gemini generateContent | `GEMINI_API_KEY`, then `GOOGLE_API_KEY` |
| `Ollama` | Ollama chat | No API key required by default |
| `DeepSeek` | OpenAI-compatible | `DEEPSEEK_API_KEY`, then `OPENAI_API_KEY` |
| `DashScope` | OpenAI-compatible | `DASHSCOPE_API_KEY`, `QWEN_API_KEY`, then `OPENAI_API_KEY` |
| `Qwen` | OpenAI-compatible | `QWEN_API_KEY`, `DASHSCOPE_API_KEY`, then `OPENAI_API_KEY` |
| `Zhipu` | OpenAI-compatible | `ZHIPU_API_KEY`, then `BIGMODEL_API_KEY` |
| `Kimi` | OpenAI-compatible | `MOONSHOT_API_KEY`, then `KIMI_API_KEY` |
| `MiniMax` | OpenAI-compatible | `MINIMAX_API_KEY` |
| `SiliconFlow` | OpenAI-compatible | `SILICONFLOW_API_KEY` |
| `Doubao` | OpenAI-compatible | `ARK_API_KEY`, then `DOUBAO_API_KEY` |
| `Nvidia` | OpenAI-compatible | `NVIDIA_API_KEY` |
| `StepFun` | OpenAI-compatible | `STEPFUN_API_KEY` |
| `MiMo` | OpenAI-compatible | `MIMO_API_KEY` |

`OpenAICompatible` requires a `base_url`. Other presets supply a default endpoint that can still be overridden.

## Create a client

```cpp
wuwe::llm_config config {
  .model = "gpt-4.1-mini",
  .timeout = 30000,
};

auto client = wuwe::make_llm_client("OpenAI", std::move(config));
const auto response = client->complete("Summarize the input.");
```

By default, normalization fills the provider endpoint and loads the first available credential from the provider's environment-variable list. Set `load_api_key_from_environment = false` when the host supplies credentials through another secret-management path.

For a custom compatible endpoint:

```cpp
wuwe::llm_config config {
  .base_url = "https://llm.example.com",
  .api_key = token,
  .model = "company-model",
};

auto client = wuwe::make_llm_client(
  "OpenAICompatible", std::move(config));
```

## Registry and capabilities

Use `list_llm_providers()`, `find_llm_provider()`, and `make_default_llm_config()` to build configuration UIs or validate deployment settings. `llm_provider_info` reports the protocol, endpoint defaults, credential names, and declared support for streaming, tools, tool choice, JSON output/schema, stop sequences, deterministic seeds, explicit cache control, reasoning summaries, multimodal input, and local runtimes.

Capabilities describe the Wuwe adapter and protocol path. A specific model or upstream account can impose narrower limits, so applications should still handle provider errors and unsupported parameters.

`llm_provider_registry` is the built-in provider catalog. It is intentionally
separate from `agent::llm::llm_client_registry`, which owns configured runtime
client instances. A `dispatching_llm_client` selects a runtime binding from
`llm_request::provider`, a configured default, or an unambiguous single binding.
Runtime binding IDs are case-sensitive and should be stable deployment identifiers,
not credentials or endpoint URLs.
The selected provider's own capability contract is checked before it receives the
request. See [Resource-aware routing](resource-routing.md) for model-and-provider
selection.
Dispatcher graphs may be composed, but request lineage rejects recursive graphs
even when a wrapper forwards the request through a worker thread. Independent
requests started by callbacks are not treated as recursion. Exceptions raised by
stream consumers remain consumer failures and do not produce a false
backend-failure telemetry event. Dispatch observers may start independent model
requests without recursively observing the events produced by those requests.

## Configuration

`llm_client_config` includes:

- base URL and chat path;
- API key policy and environment loading;
- model;
- request timeout;
- total, connect, first-event, and idle streaming timeouts;
- retry count, bounded exponential backoff, jitter, and `Retry-After` policy;
- optional OpenRouter referer and application title.

See [Resource-aware routing](resource-routing.md), [Streaming](llm-streaming.md), [Typed tools](llm-tools.md), and [HTTP backends](http-backends.md).

## Provider resilience

`resilient_llm_client` wraps one or more `llm_client` backends with a fixed-window local rate limit, bounded retry policy, circuit breaker, and ordered model fallback. Fallback is limited to retryable provider failures; authentication, invalid requests, cancellation, and other non-recovery errors are returned directly. Streaming requests may retry or fall back only before any user-visible content, reasoning, or tool-call delta has been emitted.

Built-in clients already have a local retry setting. When they are placed behind `resilient_llm_client`, set the underlying `max_retries` to zero so the wrapper owns retry accounting and avoids multiplicative retries. Use the resilience observer for backend, retry, circuit, and fallback telemetry.

`scripted_llm_client` is a public, thread-safe deterministic test client. Each scripted step can validate the request, return a response or stream events, and records consumed requests for assertions.
