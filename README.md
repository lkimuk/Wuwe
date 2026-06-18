# Wuwe

Wuwe is a C++20 framework for building agents.

## Agent Runtime

Wuwe provides a host-application-neutral agent runner for embedding agents in
desktop apps, services, CLIs, and other C++ programs. The runner supports
synchronous and async execution, cooperative cancellation with `std::stop_token`,
true OpenAI-compatible streaming, tool lifecycle callbacks, final-result
callbacks, and stateful tool providers that keep application state outside the
model-visible schema.

See [Agent Runtime](docs/agent-runtime.md), [LLM Streaming](docs/llm-streaming.md),
and [LLM Tools](docs/llm-tools.md) for the runner, streaming, and reflected-tool
APIs.

## Reasoning

Wuwe includes a Reasoning facade for selecting and combining standard agentic
reasoning patterns over the lower-level runtime modules. It supports simple
single-pass calls, ReAct-style tool use, reflect-and-retry self-correction, and
plan-and-execute workflows while emitting one unified event stream for host
applications. Results also include a structured trace and budget usage counters
for auditing model, tool, reflection, and plan-step work.

See [Reasoning](docs/reasoning.md) for the strategy boundary and API examples.

## Controlled Local Execution

Wuwe is adding a controlled local execution runtime for host-approved agent
tools such as bounded Python snippets. The design treats execution as a
high-risk capability with explicit policy, approval, audit, and replaceable
sandbox backends instead of a reasoning-mode feature or product-specific
helper. The current backend is a controlled process baseline; strong filesystem
or network isolation requires a backend that explicitly advertises those
features.

See [Controlled Local Execution Runtime](docs/execution-runtime.md) for the
architecture, threat model, Phase 1 scope, and ReArk integration plan.

## Packaging

Wuwe ships as one complete package. The release archive includes the C++ SDK,
examples, docs, and the runtime sidecars needed for turnkey PDF and Office RAG.
The repository carries a pinned Tika Server jar and Windows x64 JRE archive so
the package can be generated and used without extra release-time or client-side
downloads.

See [Packaging](docs/packaging.md) for the release layout and package script.

## LLM Providers

Wuwe's LLM layer is built around the `llm_client` interface. The default
implemented protocol client is `openai_compatible_llm_client`, which supports
OpenAI-compatible chat completions, tool calls, SSE streaming, cancellation, and
error classification. OpenAI, OpenRouter, DeepSeek, DashScope, and Qwen are
provider presets over that protocol client.

Use the factory key `OpenAICompatible` for generic compatible endpoints and
provider keys such as `OpenAI`, `OpenRouter`, `DeepSeek`, `DashScope`, and
`Qwen` for vendor defaults. Native clients are available for `Anthropic`,
`Gemini`, and `Ollama` where the provider protocol is materially different from
OpenAI-compatible chat completions.

Host applications can query `list_llm_providers()`,
`find_llm_provider()`, `make_default_llm_config()`, and
`normalize_llm_client_config()` to populate provider settings UI without
duplicating Wuwe's default endpoints or capability metadata.
They can then call `make_llm_client()` from
`<wuwe/agent/llm/llm_provider_factory.h>` to construct a provider without
including the heavy `<wuwe/wuwe.h>` aggregation header in UI-facing translation
units.

See [LLM Providers](docs/llm-providers.md) for the provider architecture and
future native-client extension path.

## HTTP Backends

Wuwe's network layer is built behind the `http_client` abstraction. The default
backend remains cpr/libcurl for mature HTTPS behavior, and Wuwe also ships a
`cpp-httplib` backend for local HTTP, comparison testing, and one-command
backend switching.

The shared contract exposes status codes, response headers, transport errors,
streaming callbacks, cancellation, redirect controls, proxy options, TLS
verification controls, custom CA paths, and trace id propagation.

Use `-DWUWE_HTTP_BACKEND=httplib` to make `default_http_client` use
`cpp-httplib`, or construct `cpr_http_client` / `httplib_http_client`
explicitly for A/B testing.

See [HTTP Backends](docs/http-backends.md) for backend selection and
verification.

## Memory Management

Wuwe includes a memory management layer for short-term conversation memory,
durable long-term memory, semantic recall, and auditable retention controls.

Implemented components include:

- In-memory, file-backed, and SQLite memory stores.
- `memory_context` for remember/recall/augment/inspect/update/delete operations.
- Runner integration for automatic request augmentation and message observation.
- Built-in `save_memory` and `search_memory` tools.
- OpenAI-compatible embeddings.
- Optional Qdrant vector index with batch upsert and metadata filters.
- Hybrid ranking across vector, lexical, priority, and recency scores.
- Audit sink, privacy filter, retention TTL, expired-memory compaction, and pending reindex reconciliation.

Basic use:

```cpp
#include <wuwe/agent/memory/memory_context.hpp>
#include <wuwe/agent/memory/in_memory_store.hpp>

int main() {
  namespace memory = wuwe::agent::memory;

  memory::memory_context context;
  context.set_scope({
    .user_id = "local-user",
    .application_id = "my-agent",
    .conversation_id = "session-1",
    .agent_id = "assistant",
  });

  context.remember_long_term(
    "The user prefers concise C++20 examples.",
    context.scope(),
    { { "topic", "preference" } });

  memory::memory_query query;
  query.scope = context.scope();
  query.kinds = { memory::memory_kind::long_term };
  query.text = "How should I answer C++ API questions?";

  const auto records = context.recall(query);
}
```

For semantic memory with Qdrant:

```powershell
$env:WUWE_QDRANT_URL="http://localhost:6333"
ctest --test-dir build-vcpkg -C Debug --output-on-failure -R memory_tests
.\build-vcpkg\examples\Debug\memory_vector_example.exe
```

See [Memory Management](docs/memory-management.md) and
[Memory Deployment](docs/memory-deployment.md) for production guidance.

## Knowledge Retrieval

Wuwe also includes a standalone RAG layer for external documents. It supports
file and directory ingestion, cited chunking, local and persistent indexes,
Qdrant and remote-vector adapters, hybrid retrieval, reranking, caching,
observability, grounding checks, benchmark utilities, and `search_knowledge`
tool exposure.

See [Knowledge Retrieval](docs/knowledge-retrieval.md) for current status,
pipeline examples, deployment notes, and remaining production work.

## Planning

Wuwe includes a modular Planning layer for goal-driven task decomposition and
execution control. It provides static and LLM-backed planners, tool-aware plan
generation, plan validation, function and tool executors, retry policy,
replanning hooks, observer events, JSON serialization for checkpoints,
checkpoint resume, single-run step budgets, cancellation checks, approval gates,
plan stores, trace events, parallel ready-step execution, timeout marking,
typed JSON I/O, artifacts, agent handoff executors, policy hooks, optional
memory recording, and an optional Reflection gate for retry/revise/replan/block
closed loops. Planning lives independently from flow, runner, memory, MCP, and
RAG modules, so existing behavior is unchanged unless an application explicitly
creates a `plan_runner`. The Planning core is complete for embedded agent
runtimes; deployment-platform work such as distributed workers, leases,
database-backed stores, external approval systems, and telemetry exporters is
tracked in the Planning roadmap.

See [Planning](docs/planning.md) for API boundaries and extension points.

## Observability

Wuwe includes a small shared observability contract in
`<wuwe/agent/core/observability.hpp>`. Modules can publish normalized
`agent_event` records to in-memory, fanout, or JSONL sinks. Knowledge Retrieval
and MCP host telemetry currently provide adapters into this shared sink while
keeping their module-specific event types.

## Reflection

Wuwe includes a Reflection layer for evaluating existing outputs, tool results,
plan step results, RAG answers, and final answers against structured rubrics.
It provides deterministic rule checks, LLM-backed reflection, composite
reflection, policy action mapping, runner events, JSON codecs, and in-memory or
file-backed reflection history. Reflection is independent from Planning, but
Planning can now consume its action recommendations through `plan_reflection_gate`
for retry, revise, replan, block, or escalation flows.

The real LLM example is `reflection_example`:

```powershell
$env:OPENROUTER_API_KEY = "your_api_key"
$env:OPENROUTER_CHAT_MODEL = "openai/gpt-oss-120b:free"
cmake --build build-mcp --config Debug --target reflection_example
.\build-mcp\examples\Debug\reflection_example.exe
```

See [Reflection](docs/reflection.md) for API boundaries, current observability,
and future adapters.

## Model Context Protocol

Wuwe includes an MCP module for exposing existing Wuwe tool providers,
resources, roots, and prompts through JSON-RPC, `initialize`, `tools/list`,
`tools/call`, `resources/list`, `resources/read`, `roots/list`, `prompts/list`,
`prompts/get`, Content-Length framed stdio transport, a small stdio client, an
external process stdio client, a lightweight multi-server host runtime with
stderr capture, async dispatch, health checks, bounded restart policy, and basic
telemetry, structured runtime events, JSONL/Prometheus/OpenTelemetry-style
telemetry export, MCP aggregation gateway, restart backoff and circuit breaker
protection, an application-level HTTP adapter,
project-local and user-level MCP config discovery, config-driven child process environment overrides,
notifications, lifecycle tracking, async task
helpers, sampling and elicitation request forwarding, pagination, and optional
access/audit policy.

See [MCP Module](docs/mcp.md) for API usage and
[MCP Host Compatibility](docs/mcp-host-compatibility.md) for desktop host setup.

## Install

```bash
cmake -S . -B build
cmake --build build --config Release
cmake --install build --config Release --prefix <install-prefix>
```

The install prefix includes Wuwe's bundled runtime sidecars under `runtime/`, so
applications can consume the install directory directly without separately
installing Java or starting a document parser.

## Use From Another Project

Add the install prefix to `CMAKE_PREFIX_PATH`, then use `find_package`:

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_app LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

list(APPEND CMAKE_PREFIX_PATH "<install-prefix>")

find_package(wuwe CONFIG REQUIRED)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE wuwe::wuwe)
```

Example:

```cpp
#include <wuwe/net/default_http_client.h>

int main() {
    wuwe::agent::default_http_client client;

    wuwe::agent::http_request request;
    request.method = "GET";
    request.url = "https://api.openai.com";

    const wuwe::agent::http_response response = client.send(request);
    return response.error_code.value();
}
```
