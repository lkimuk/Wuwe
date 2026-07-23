---
id: llm-providers
title: LLM providers
description: Configure built-in cloud and local model clients through one interface.
---

# LLM providers

Wuwe normalizes provider configuration, requests, responses, streaming events, tool calls, usage, retries, and errors behind `llm_client`.

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

Use `list_llm_providers()`, `find_llm_provider()`, and `make_default_llm_config()` to build configuration UIs or validate deployment settings. `llm_provider_info` reports the protocol, endpoint defaults, credential names, and declared support for streaming, tools, tool choice, JSON output, reasoning summaries, multimodal input, and local runtimes.

Capabilities describe the Wuwe adapter and protocol path. A specific model or upstream account can impose narrower limits, so applications should still handle provider errors and unsupported parameters.

## Configuration

`llm_client_config` includes:

- base URL and chat path;
- API key policy and environment loading;
- model;
- request timeout;
- total, connect, first-event, and idle streaming timeouts;
- retry count and backoff;
- optional OpenRouter referer and application title.

See [Streaming](llm-streaming.md), [Typed tools](llm-tools.md), and [HTTP backends](http-backends.md).
