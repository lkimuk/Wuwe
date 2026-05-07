# Wuwe

Wuwe is a C++20 framework for building agents.

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
