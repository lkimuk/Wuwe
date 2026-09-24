---
id: llm-providers
title: LLM providers
description: Configure built-in cloud and local model clients through one interface.
---

# LLM providers

For callers adopting model discovery and the new presets, see the
[中文接入指南](llm-integration-update-zh.md).

Codex account login and account-scoped discovery use a separate
[OAuth API](codex-oauth-integration-zh.md). Account-bound generation, streaming and
function tools use [codex_llm_client](codex-generation-zh.md), which implements `llm_client`.

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

### Kimi, MiniMax, SiliconFlow, and Doubao

These presets reuse the Chat Completions adapter and support normal responses,
streaming, and tool calls. They do not select a model automatically or pin a
model catalog that can become stale. Set `config.model` or `request.model` to the
model you intend to use.

| ID | Default base URL | Chat path | Model-list path appended to base URL |
| --- | --- | --- | --- |
| `Kimi` | `https://api.moonshot.cn` | `/v1/chat/completions` | `/v1/models` |
| `MiniMax` | `https://api.minimaxi.com` | `/v1/chat/completions` | `/v1/models` |
| `SiliconFlow` | `https://api.siliconflow.cn` | `/v1/chat/completions` | `/v1/models` |
| `Doubao` | `https://ark.cn-beijing.volces.com/api/v3` | `/chat/completions` | `/models` |

```cpp
wuwe::llm_client_config config {
  .model = selected_model_id,
}; // Loads MOONSHOT_API_KEY, then KIMI_API_KEY.
auto client = wuwe::make_llm_client("Kimi", config);

wuwe::llm_request request;
request.messages.push_back({ .role = "user", .content = "Summarize this document." });
request.temperature = 1.0; // Choose sampling parameters allowed by your model.
auto response = client->complete(request);

auto models = wuwe::list_llm_models("Kimi", config);
```

Public `kimi_llm_client`, `minimax_llm_client`, `siliconflow_llm_client`, and
`doubao_llm_client` classes also accept an injected `std::shared_ptr<http_client>`.
They use the same defaults and credential policy as the factory.

- **Credentials:** these four presets only read the dedicated environment
  variables listed above; they never fall back to `OPENAI_API_KEY` or
  `OPENROUTER_API_KEY`. Explicit `api_key` wins, and
  `load_api_key_from_environment = false` disables environment loading.
- **Regional endpoints:** for international accounts, override `base_url` with
  `https://api.moonshot.ai`, `https://api.minimax.io`, or
  `https://api.siliconflow.com` respectively. These are base URLs without `/v1`,
  because each preset already supplies `/v1/chat/completions`. Credentials must
  belong to the selected service/region. No endpoint fallback is performed.
- **Kimi:** assistant `reasoning_content` is replayed for thinking-model tool
  continuations. Explicit `thinking_mode` uses `thinking.type=enabled/disabled`;
  the default leaves the setting to the model. Model-specific temperature and
  thinking restrictions remain upstream constraints; Wuwe does not silently
  replace a requested sampling value.
- **MiniMax:** native Chat Completions returns thinking inside `<think>...</think>`
  in `content` by default. Wuwe preserves that content for multi-turn replay.
  This preset does not enable `reasoning_split` or advertise separate reasoning
  events, explicit tool choice, JSON output/schema, or stop controls. It supports
  ordinary automatic tool calls. Unsupported declared controls are rejected
  before network dispatch.
- **SiliconFlow:** use the exact provider-qualified model ID returned by the
  service, including any `Pro/` prefix. Model capabilities vary across the
  catalog. Assistant `reasoning_content` is retained for reasoning-model tool
  continuations; model-specific thinking switches are not inferred.
- **Doubao:** use the model ID or your Ark inference endpoint ID (`ep-...`) as
  `model`, as required by your deployment. Wuwe preserves it verbatim. Model
  discovery queries the runtime `/api/v3/models` route; it is not an Ark
  administrative endpoint/deployment inventory API. If the account or runtime
  does not expose this route, handle the discovery error and configure the
  model/endpoint ID manually.

These defaults cover the normal API-platform endpoints. Coding Plan / Token Plan
credentials and routes are separate configurations and are not implicitly
substituted. Model-list results do not establish permissions to call a model.

Endpoint references: [CC Switch provider presets](https://github.com/farion1231/cc-switch/blob/main/src/config/openclawProviderPresets.ts),
[Kimi documentation](https://platform.moonshot.ai/docs/overview),
[MiniMax Chat Completions](https://platform.minimax.io/docs/api-reference/text-openai-api),
[MiniMax model listing](https://platform.minimax.io/docs/api-reference/models/openai/list-models),
[SiliconFlow Chat Completions](https://docs.siliconflow.cn/cn/api-reference/chat-completions/chat-completions),
and [Volcengine Ark documentation](https://www.volcengine.com/docs/82379).

### NVIDIA NIM, StepFun, and Xiaomi MiMo

These presets use the existing Chat Completions client, with dedicated API keys
and the same factory, streaming, tool-call, and model-discovery interfaces:

| ID | Default base URL | Chat path | Model-list path |
| --- | --- | --- | --- |
| `Nvidia` | `https://integrate.api.nvidia.com` | `/v1/chat/completions` | `/v1/models` |
| `StepFun` | `https://api.stepfun.com` | `/v1/chat/completions` | `/v1/models` |
| `MiMo` | `https://api.xiaomimimo.com` | `/v1/chat/completions` | `/v1/models` |

For example, `make_llm_client("MiMo", config)` creates a configured client, and
`list_llm_models("MiMo", config)` queries the same service. Set `config.model`
explicitly; no model is chosen automatically. The public `nvidia_llm_client`,
`stepfun_llm_client`, and `mimo_llm_client` classes accept an injected HTTP client
as well. Only the dedicated environment variable in the table is consulted;
unrelated OpenAI/OpenRouter credentials are never used as fallbacks.

- **NVIDIA NIM:** the default targets NVIDIA's hosted API catalog. Keep the full
  model identifier, such as its vendor prefix. Model capabilities and thinking
  switches differ across hosted models; the preset does not inject a common
  `thinking` switch or NVIDIA-specific `chat_template_kwargs`. Self-hosted NIM
  addresses can be supplied explicitly using `base_url` and, when needed,
  `chat_completions_path`.
- **StepFun:** the default is the ordinary China API platform. An international
  account can use `base_url = "https://api.stepfun.ai"`. Step Plan is a separate
  subscription route and is not selected automatically; configure its base URL
  explicitly, without duplicating the preset's `/v1` path. Model-specific
  thinking controls are left to the provider default.
- **MiMo:** the ordinary API platform accepts Bearer authentication for both
  generation and model listing. The preset uses that documented format instead
  of requiring a special `api-key` header. `thinking_mode` maps to
  `thinking.type=enabled/disabled`; the default omits the switch.
  `max_output_tokens` maps to **`max_completion_tokens`** for both normal and
  streaming calls. MiMo only guarantees automatic tool selection, so leave
  `tool_choice` unset; explicit tool-choice controls are rejected before dispatch.
  Some MiMo thinking models override sampling parameters upstream (the current
  V2.5 family documents temperature 1.0). Wuwe preserves the requested value and
  does not promise deterministic sampling. Token Plan uses a separate endpoint
  and key and is not implicitly substituted.

All three presets preserve assistant `reasoning_content` in tool continuations
and expose separate reasoning output when the upstream provides it. Model-list
queries do not establish model permissions or capabilities; callers should handle
discovery errors and keep manual model selection available.

References: [NVIDIA API catalog](https://docs.api.nvidia.com/nim/reference/llm-apis),
[NVIDIA model listing](https://docs.api.nvidia.com/nim/reference/models-1),
[StepFun official integration examples](https://github.com/stepfun-ai/Step-3.5-Flash#readme),
[MiMo Chat Completions](https://mimo.mi.com/static/docs/api/chat/openai-api.md),
[MiMo model listing](https://mimo.mi.com/static/docs/api/model/list-models.md),
and [MiMo thinking/tool continuation](https://mimo.mi.com/static/docs/quick-start/usage-guide/text-generation/deep-thinking.md).

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

## Discover available models

`list_llm_models()` queries the configured endpoint without invoking a model or
changing the provider, runtime registry, or router. Include
`<wuwe/agent/llm/llm_model_discovery.h>` (also exported by `<wuwe/wuwe.h>`).

```cpp
wuwe::llm_client_config config {
  .base_url = "https://gateway.example.com/v1",
  .api_key = token,
  .load_api_key_from_environment = false,
  .timeout = 10'000,
};

const auto result = wuwe::list_llm_models("OpenAICompatible", config);
if (result.error_code) {
  // result.error_code is a Wuwe llm_error_code; http_status and
  // transport_error retain structured diagnostic information.
  return;
}
for (const auto& model : result.models) {
  // model.id can be used as config.model; display_name is optional.
}
```

The function uses the same provider defaults and environment-key policy as
`make_llm_client()`. It does not require `config.model`. The provider ID must be a
built-in registry ID; use `OpenAICompatible` for a custom compatible service.

| Format | Default endpoint | Authentication | Pagination |
| --- | --- | --- | --- |
| OpenAI compatible | Sibling `models` route of the chat path | Bearer token | `has_more` / `last_id` → `after` when supplied |
| Anthropic | `/v1/models` | `x-api-key`, `anthropic-version` | `has_more` / `last_id` → `after_id` |
| Gemini | `/v1beta/models` | `x-goog-api-key` | `nextPageToken` → `pageToken` |
| Ollama | `/api/tags` | Optional bearer token | Single list of installed models |

Generation (including streaming) and discovery share endpoint construction,
preserving base-URL prefixes and avoiding duplicate version suffixes:
`https://host/v1/` becomes `https://host/v1/models` for discovery and
`https://host/v1/chat/completions` for OpenAI-compatible generation. Native
Anthropic `/v1`, Gemini `/v1` or `/v1beta`, and Ollama `/api` prefixes are also
preserved. The built-in
Zhipu preset uses `/api/paas/v4/models`. A nonstandard chat route that does not end
in `/chat/completions` requires an explicit model-list path. Base URLs must be
HTTP(S), without embedded credentials, queries, or fragments.

For a gateway whose discovery format or route differs from generation:

```cpp
const auto result = wuwe::list_llm_models("Anthropic", config, {
  .format = wuwe::llm_model_list_format::openai,
  .models_path = "/openai/v1/models",
});
```

`models_path` is appended to `base_url` after trimming trailing slashes; it must
start with a single `/` and contain no query or fragment. In the example above,
set `base_url` to the gateway root. `format` selects both response parsing and
authentication headers. This does not change generation configuration. The
function never probes alternative endpoints or follows HTTP redirects.

The result contains **all pages or an error**, never a successful partial list.
Duplicate IDs keep their first occurrence and its metadata, in server order.
Gemini's `models/` resource prefix is removed; other IDs are preserved. Discovery
does not infer context windows, prices, capabilities, account permissions, or
filter out embedding models. A listed model is not a guarantee of callable access.
Keep manual model entry available for services that do not implement discovery.

The compatible parser also accepts Zhipu-style `models[].slug` catalogs when
`data` is absent. An explicit `models_path` selects an alternate catalog route;
it does not enable Responses generation. If `data` exists it takes precedence,
and malformed `data` is rejected rather than hidden by a fallback.

For a public catalog, set `require_api_key = false`, leave `api_key` empty, and
set `load_api_key_from_environment = false` to send an anonymous request. The
server still determines whether authentication is necessary.

Operational contract:

- `config.timeout` must be positive and is a total deadline across pages and
  parsing. Generation retry settings are not used; discovery makes no automatic
  retries. Applications may retry a failed query explicitly.
- `llm_model_list_options` bounds pages (100), unique models (10,000), and each
  response body (8 MiB) by default. All limits must be positive. Exceeding a limit
  returns `model_list_limit_exceeded`; malformed pages or repeated pagination
  cursors return `invalid_response`.
- An empty valid list succeeds. HTTP 404/405/501 return `unsupported_capability`;
  401/403 return `authentication_failed`; 429 returns `rate_limited`. Other HTTP,
  timeout, transport, and malformed-response errors remain distinguishable. No
  upstream error bodies or credentials are included in error results.
- Pass `std::stop_token` as the last argument to cancel. The overload accepting
  `http_client&` borrows a transport for the call, enabling deterministic tests
  and custom networking. It uses `send_stream()` to bound collection and forward
  cancellation. Custom transports must honor the callback and stop token;
  cancellation latency follows the selected transport. Transport exceptions are
  propagated, like the underlying HTTP interface.
- There is no global cache or background work. Concurrent calls are independent;
  a caller sharing a custom transport is responsible for its thread safety.

See `examples/src/llm_models_example.cpp` for a runnable CLI example. It loads
credentials from the provider's environment variables.

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
