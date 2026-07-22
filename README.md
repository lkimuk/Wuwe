# Wuwe

Wuwe is a C++20 framework for building tool-using, stateful, and auditable AI
agents in native applications, services, and command-line programs.

It provides a modular runtime for LLM access, typed tools, reasoning, planning,
reflection, memory, retrieval-augmented generation (RAG), Model Context Protocol
(MCP), controlled execution, and observability. Applications can use individual
modules or compose them through the higher-level reasoning and planning APIs.

Current version: **0.1.0**

## Release support

Wuwe is designed as a cross-platform framework. The 0.1.0 release has the
following support boundary:

| Platform | Source build | Official binary package | Release status |
| --- | --- | --- | --- |
| Windows x64 | Supported | `wuwe-0.1.0-windows-x64.zip` | Verified release platform |
| Linux x64 (Ubuntu 24.04) | Supported | `wuwe-0.1.0-linux-x64.tar.gz` | Release CI profile |
| macOS | Intended and kept portable | Not provided in 0.1.0 | Not yet CI-certified |

Platform-neutral modules are separated from operating-system-specific
capabilities. In particular, the restricted-process backend is currently a
Windows implementation; Linux and macOS sandbox backends remain future work.

## Capabilities

| Module | Responsibility |
| --- | --- |
| Agent runtime | Synchronous and asynchronous runs, streaming, cancellation, tool lifecycle events, and host-owned state |
| LLM providers | OpenAI-compatible APIs plus native Anthropic, Gemini, and Ollama clients |
| Tools | Typed/reflected tool schemas, tool dispatch, result handling, and stateful providers |
| Reasoning | Direct calls, ReAct tool loops, reflect-and-retry, and plan-and-execute strategies with unified traces and budgets |
| Planning | Static and LLM-backed planning, dependencies, retries, replanning, checkpoints, approvals, policy hooks, and parallel ready-step execution |
| Reflection | Deterministic and LLM-backed evaluation with retry, revise, replan, block, and escalation actions |
| Memory | In-memory, file-backed, and optional SQLite persistence; embeddings, hybrid ranking, retention, and optional Qdrant indexing |
| Knowledge | File and directory ingestion, Tika document parsing, chunking, persistent indexes, hybrid retrieval, reranking, grounding, and citations |
| MCP | Server, client, external-process host, multi-server aggregation, stdio/HTTP adapters, lifecycle management, policy, and telemetry |
| Controlled execution | Capability-aware execution, approvals, audit records, filesystem policy, and an opt-in Windows restricted-process backend |
| Observability | Shared structured events, JSONL sinks, telemetry adapters, traces, and usage accounting |

## LLM providers

The default provider abstraction is `llm_client`. Wuwe includes presets for
OpenAI, OpenRouter, DeepSeek, DashScope, and Qwen through the OpenAI-compatible
protocol, together with native clients for Anthropic, Gemini, and Ollama.

Provider metadata and defaults are available through the registry APIs, so host
applications do not need to duplicate endpoints or capability declarations.

See [LLM Providers](docs/llm-providers.md),
[LLM Streaming](docs/llm-streaming.md), and [LLM Tools](docs/llm-tools.md).

## Build

### Verified Windows x64 build

Requirements:

- Visual Studio 2022 with the C++ desktop workload
- CMake 3.25 or newer for the included presets
- Git
- vcpkg, referenced through `VCPKG_ROOT`

```powershell
$env:VCPKG_ROOT = "D:\tools\vcpkg"

cmake --preset windows-vcpkg
cmake --build --preset windows-vcpkg-release
ctest --preset windows-vcpkg-release
```

The preset uses the repository's pinned `vcpkg.json` baseline. It restores
SQLite into the build directory without installing it system-wide. The official
Windows profile uses cpr/libcurl with Schannel and does not link OpenSSL.

An explicit OpenSSL variant is also available:

```powershell
cmake --preset windows-vcpkg-openssl
cmake --build --preset windows-vcpkg-openssl-release
ctest --preset windows-vcpkg-openssl-release
```

### Linux x64 build

Requirements:

- Ubuntu 24.04 or a compatible x64 Linux distribution
- GCC 13 or Clang with C++20 support
- CMake 3.25 or newer, Make, and Git
- vcpkg, referenced through `VCPKG_ROOT`

```bash
export VCPKG_ROOT="$HOME/vcpkg"

cmake --preset linux-vcpkg
cmake --build --preset linux-vcpkg-release
ctest --preset linux-vcpkg-release
```

The Linux release profile restores pinned OpenSSL and SQLite packages through
the same manifest used on Windows. It also installs a separate Linux x64
Temurin runtime for Tika; no Windows runtime binaries are reused.

### Generic CMake build

For other toolchains and platforms:

```bash
cmake -S . -B build -DWUWE_TLS_BACKEND=auto -DWUWE_SQLITE_MODE=auto
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix <install-prefix>
```

The generic path remains useful for custom Linux toolchains and macOS. macOS is
not part of the 0.1.0 release certification matrix.

See [Dependencies](docs/dependencies.md) for TLS, SQLite, runtime, and package
consumer requirements.

## Minimal example

```cpp
#include <iostream>

#include <wuwe/wuwe.h>

int main() {
  wuwe::llm_config config {
    .model = "gpt-4.1-mini",
  };

  wuwe::llm_client_factory factory;
  auto client = factory.create_shared("OpenAI", config);
  const auto response = client->complete("Explain RAII in one paragraph.");

  if (!response) {
    std::cerr << response.error_code.message() << '\n';
    return 1;
  }

  std::cout << response.content << '\n';
}
```

Set `OPENAI_API_KEY` before running the example. Other providers can be
selected through the same factory.

## Consume an installed SDK

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_agent LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(wuwe CONFIG REQUIRED)

add_executable(my_agent main.cpp)
target_link_libraries(my_agent PRIVATE wuwe::wuwe)
```

Add the Wuwe installation prefix to `CMAKE_PREFIX_PATH` when it is not installed
in a standard CMake search location. The exported package requests only the
public dependencies enabled in that specific Wuwe build.

## Package

Generate a release archive after completing the matching release build and
tests.

Windows x64:

```powershell
.\tools\package-wuwe.ps1 -BuildDir build-vcpkg -Configuration Release
```

The archive is written to:

```text
dist/wuwe-0.1.0-windows-x64.zip
```

Linux x64:

```bash
bash ./tools/package-wuwe.sh \
  --build-dir build-linux-vcpkg \
  --configuration Release
```

```text
dist/wuwe-0.1.0-linux-x64.tar.gz
```

It contains the static SDK, CMake package files, examples, documentation, the
pinned Tika server, and a platform-matched Java runtime. `manifest.json` records
the actual HTTP, TLS, SQLite, and runtime capabilities, while
`checksums.sha256` protects packaged files.

Windows x64 and Linux x64 package builds select separate, pinned Temurin 21 JRE
archives. The Tika JAR is shared, but a JRE binary is never reused across
operating systems.

SQLite and OpenSSL remain public link dependencies when enabled; they are not
silently copied into the SDK archive. See [Packaging](docs/packaging.md) for the
complete package contract.

## Production boundaries

Wuwe reports capabilities explicitly and does not treat optional integrations
as guaranteed:

- The default controlled-process backend is not a strong sandbox. Strong
  isolation requires a backend that advertises and enforces the requested
  filesystem, network, identity, and resource restrictions.
- SQLite persistence in 0.1.0 is intended for local, primarily single-process
  workloads. Its knowledge-vector search is a deterministic linear scan, not an
  approximate-nearest-neighbor index.
- Qdrant is an optional external service for larger semantic indexes.
- Platform-matched Temurin 21 archives are used for Windows x64 and Linux x64;
  both release profiles verify the installed runtime and package layout.
- macOS has not yet received release CI or binary-package certification.

## Documentation

- [Agent Runtime](docs/agent-runtime.md)
- [Reasoning](docs/reasoning.md)
- [Planning](docs/planning.md)
- [Reflection](docs/reflection.md)
- [Memory Management](docs/memory-management.md)
- [Knowledge Retrieval](docs/knowledge-retrieval.md)
- [Model Context Protocol](docs/mcp.md)
- [Controlled Local Execution](docs/execution-runtime.md)
- [HTTP Backends](docs/http-backends.md)
- [Dependencies](docs/dependencies.md)
- [Packaging](docs/packaging.md)

## License

Wuwe is distributed under the terms in [LICENSE](LICENSE).
