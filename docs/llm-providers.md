# LLM Providers

Wuwe separates provider protocols from provider presets.

## Protocol Clients

Protocol clients implement a wire protocol and should not encode a specific
vendor's product defaults unless those defaults are part of the protocol.

### `openai_compatible_llm_client`

`openai_compatible_llm_client` implements OpenAI-compatible chat completions:

- `POST /v1/chat/completions`,
- OpenAI-style chat messages,
- OpenAI-style tool calls,
- OpenAI-compatible SSE streaming,
- OpenAI-style usage and error payload parsing.

Use it for OpenAI-compatible services such as OpenAI, vLLM, LM Studio,
OpenRouter-compatible gateways, DeepSeek-compatible endpoints, Qwen-compatible
gateways, and local servers that expose the same API shape.

The client is protocol-neutral. It does not add OpenRouter-specific headers by
default.

Factory key:

```cpp
auto client = factory.create_shared("OpenAICompatible", config);
```

## Provider Presets

Provider presets configure a protocol client for a concrete vendor or gateway.
They are the right place for default base URLs, provider-specific environment
variables, and provider-specific optional headers.

Applications should discover those defaults through the provider registry
instead of hard-coding a parallel provider table:

```cpp
#include <wuwe/agent/llm/llm_provider_registry.h>

for (const auto& provider : wuwe::list_llm_providers()) {
  std::cout << provider.id << " -> " << provider.default_base_url << "\n";
}

const auto* info = wuwe::find_llm_provider("OpenAI");
if (info && !info->base_url_required) {
  auto config = wuwe::make_default_llm_config(*info);
  config.model = "gpt-4.1-mini";
}
```

The registry exposes:

- stable factory/provider ids,
- display names for UI surfaces,
- protocol type,
- default base URL and whether the URL is required,
- API-key policy and supported environment variable names,
- streaming/tool/local-runtime capabilities,
- optional model recommendations when Wuwe carries a stable model catalog for
  that provider.

Use `normalize_llm_client_config(provider_id, config)` when a host wants to
apply Wuwe defaults while preserving explicit user overrides:

```cpp
wuwe::llm_client_config config;
config.base_url = user_override_url;
config.model = user_selected_model;

auto normalized = wuwe::normalize_llm_client_config("OpenAI", std::move(config));
```

Construct provider clients through the narrow provider factory API:

```cpp
#include <wuwe/agent/llm/llm_provider_factory.h>

auto client = wuwe::make_llm_client("OpenAI", std::move(config));
```

`<wuwe/wuwe.h>` remains a convenience aggregation header, but it no longer
performs factory registration as a header side effect. Host applications such as
desktop apps should prefer the narrow `llm_provider_registry.h`,
`llm_provider_factory.h`, `llm_client.h`, and `llm_config.h` headers for their
settings and provider-selection code.

### `openai_llm_client`

`openai_llm_client` is the OpenAI preset over the OpenAI-compatible protocol
client.

It provides:

- default base URL `https://api.openai.com`,
- `OPENAI_API_KEY` environment loading,
- the same tool-call, streaming, cancellation, retry, and error behavior as
  `openai_compatible_llm_client`.

Factory key:

```cpp
auto client = factory.create_shared("OpenAI", config);
```

### `openrouter_llm_client`

`openrouter_llm_client` is an OpenRouter preset over the OpenAI-compatible
protocol client.

It provides:

- default base URL `https://openrouter.ai/api` when none is supplied,
- `OPENROUTER_API_KEY` preference for OpenRouter integrations,
- default `HTTP-Referer` and `X-Title` headers for OpenRouter attribution.

The attribution headers are only filled when the fields are unset. Set
`referer_url` or `app_title` to an explicit empty string to suppress the
corresponding header.

Factory key:

```cpp
auto client = factory.create_shared("OpenRouter", config);
```

### `deepseek_llm_client`

`deepseek_llm_client` is a DeepSeek preset over the OpenAI-compatible protocol
client.

It provides:

- default base URL `https://api.deepseek.com`,
- `DEEPSEEK_API_KEY` preference with `OPENAI_API_KEY` fallback.

Factory key:

```cpp
auto client = factory.create_shared("DeepSeek", config);
```

### `dashscope_llm_client` / `qwen_llm_client`

`dashscope_llm_client` and `qwen_llm_client` are DashScope/Qwen presets over
the OpenAI-compatible protocol client.

They provide:

- default base URL `https://dashscope.aliyuncs.com/compatible-mode`,
- `DashScope`: `DASHSCOPE_API_KEY` preference, then `QWEN_API_KEY`, then
  `OPENAI_API_KEY`,
- `Qwen`: `QWEN_API_KEY` preference, then `DASHSCOPE_API_KEY`, then
  `OPENAI_API_KEY`.

Factory keys:

```cpp
auto dashscope = factory.create_shared("DashScope", config);
auto qwen = factory.create_shared("Qwen", config);
```

## Native Provider Clients

Native clients implement providers whose protocol semantics are not cleanly
captured by OpenAI-compatible chat completions.

Native clients honor the shared `llm_client_config` retry settings
(`max_retries` and `retry_backoff_ms`) for retryable failures before any stream
output is emitted. Streaming clients also treat missing terminal provider
events as `llm_error_code::invalid_response` instead of silently returning a
partial success.

### `anthropic_llm_client`

`anthropic_llm_client` implements Anthropic's Messages API:

- `POST /v1/messages`,
- `x-api-key` authentication,
- `anthropic-version` header,
- text content blocks,
- tool declarations and `tool_use` parsing,
- SSE message streaming.

Factory key:

```cpp
auto client = factory.create_shared("Anthropic", config);
```

### `gemini_llm_client`

`gemini_llm_client` implements Gemini's native generate content API:

- `POST /v1beta/models/{model}:generateContent`,
- `:streamGenerateContent?alt=sse` streaming,
- `x-goog-api-key` authentication,
- text parts,
- function declarations and `functionCall` parsing.

Factory key:

```cpp
auto client = factory.create_shared("Gemini", config);
```

### `ollama_llm_client`

`ollama_llm_client` implements Ollama's native local chat API:

- `POST /api/chat`,
- local default base URL `http://localhost:11434`,
- line-delimited JSON streaming,
- optional bearer auth for proxied deployments,
- text and tool-call parsing.

Ollama defaults `require_api_key` to false.

Factory key:

```cpp
auto client = factory.create_shared("Ollama", config);
```

## Environment Variables

`llm_client_config` does not store an API key by default. When
`load_api_key_from_environment` is true and `api_key` is empty, the client
loads an environment key during construction.

The generic `openai_compatible_llm_client` uses
`llm_client_config::load_api_key_from_env()`, which checks `OPENAI_API_KEY`
first, then `OPENROUTER_API_KEY`.

The OpenRouter preset uses `load_openrouter_api_key_from_env()`, which checks
`OPENROUTER_API_KEY` first, then `OPENAI_API_KEY`.

Other provider-specific helpers are:

- `load_anthropic_api_key_from_env()` -> `ANTHROPIC_API_KEY`,
- `load_gemini_api_key_from_env()` -> `GEMINI_API_KEY`, then `GOOGLE_API_KEY`,
- `load_deepseek_api_key_from_env()` -> `DEEPSEEK_API_KEY`, then `OPENAI_API_KEY`,
- `load_dashscope_api_key_from_env()` -> `DASHSCOPE_API_KEY`, then
  `QWEN_API_KEY`, then `OPENAI_API_KEY`.

Local OpenAI-compatible servers can set:

```cpp
config.require_api_key = false;
```

Hosts that need strict no-authorization behavior, even when environment keys
exist, should also set:

```cpp
config.load_api_key_from_environment = false;
```

## Future Native Clients

Add native clients when a provider's protocol semantics are not a clean
OpenAI-compatible fit. The current native first-priority clients are Anthropic,
Gemini, and Ollama. Future families can include Azure OpenAI, Bedrock, Vertex
AI, or other cloud provider adapters when their auth and request lifecycle
should be owned by Wuwe.

Each native client should implement `llm_client` directly, preserve streaming
and cancellation semantics, classify provider-specific errors into
`llm_error_code`, and expose a provider preset factory key.

## Design Rules

- Name protocol clients after protocols, not vendors.
- Name provider presets after vendors or hosted gateways.
- Keep tool-call, streaming, cancellation, retry, and error behavior testable at
  the protocol-client layer.
- Keep provider defaults thin and explicit.
- Preserve old provider preset names when adding a more accurate protocol name.
