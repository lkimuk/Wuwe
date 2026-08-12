![Wuwe banner](docs/assets/brand/banner.png)

<p align="center">
  <a href="./LICENSE">
    <img src="https://img.shields.io/github/license/lkimuk/Wuwe.svg" alt="License">
  </a>
  <img src="https://img.shields.io/github/languages/top/lkimuk/Wuwe.svg" alt="Language">
  <img src="https://img.shields.io/github/last-commit/lkimuk/Wuwe.svg" alt="Last commit">
</p>

<p align="center">
  <a href="https://github.com/lkimuk/Wuwe/actions/workflows/ci-windows.yml">
    <img src="https://github.com/lkimuk/Wuwe/actions/workflows/ci-windows.yml/badge.svg?branch=main" alt="Windows CI">
  </a>
  <a href="https://github.com/lkimuk/Wuwe/actions/workflows/ci-linux.yml">
    <img src="https://github.com/lkimuk/Wuwe/actions/workflows/ci-linux.yml/badge.svg?branch=main" alt="Linux CI">
  </a>
  <a href="https://github.com/lkimuk/Wuwe/actions/workflows/ci-macos.yml">
    <img src="https://github.com/lkimuk/Wuwe/actions/workflows/ci-macos.yml/badge.svg?branch=main" alt="macOS CI">
  </a>
</p>

<p align="center">
  <strong>Documentation:</strong>
  <a href="https://lkimuk.github.io/Wuwe/">https://lkimuk.github.io/Wuwe/</a>
</p>

# Wuwe

Wuwe is a C++20 framework for building tool-using, stateful, and auditable AI agents in native applications, services, and command-line programs.

It provides independently usable modules for model access, resource-aware routing, typed tools, reusable Skills, reasoning with Best-of-N selection, reflection, planning, multi-agent collaboration, A2A, typed fan-out/fan-in orchestration, memory, retrieval-augmented generation, guardrails, evaluation, offline learning and adaptation, controlled exploration and discovery, MCP, networking, policy, approvals, audit, observability, and controlled execution.

## Release support

Version 1.0.0 is verified on:

| Platform | Package | Status |
| --- | --- | --- |
| Windows x64 / Visual Studio 2022 | `wuwe-1.0.0-windows-x64.zip` | Certified release profile |
| Ubuntu 24.04 Linux x64 | `wuwe-1.0.0-linux-x64.tar.gz` | Certified release profile |
| macOS 14+ Apple Silicon / AppleClang | `wuwe-1.0.0-macos-arm64.tar.gz` | Certified source and package profile |

The default `controlled_process` backend is supported on all three platforms. The opt-in
`restricted_process` backend uses AppContainer on Windows and a compiled deny-default Seatbelt
profile launched through macOS's SIP-protected system sandbox utility. Linux reports that stronger
capability as unavailable rather than silently
weakening its policy.

Wuwe 1.x follows semantic versioning for its documented public C++ source API.
Consumers should rebuild when upgrading the SDK; cross-release C++ ABI compatibility
is not promised. See [Versioning and compatibility](docs/versioning.md), the
[1.0 migration guide](docs/migration-1.0.md), and [CHANGELOG.md](CHANGELOG.md).

## Capabilities

| Area | Included capabilities |
| --- | --- |
| Models and tools | OpenAI-compatible, Anthropic, Gemini, and Ollama clients; capability-aware model routing; token and cost budgets; streaming; typed schemas and dispatch |
| Agent runtime | Tool loops, callbacks, cancellation, reasoning modes, team sessions, skill dispatch, parallel collaboration, consensus, plans, and traces |
| Skills | Strict manifests, immutable packages, SemVer dependency resolution, governed activation, trust-aware instruction projection, and explicit runtime adapters |
| Agent interoperability | A2A Agent Cards, Messages, Tasks, Artifacts, discovery, JSON-RPC, HTTP transport, and local/remote team adapters |
| Orchestration | Typed chains, context-aware cancellation, bounded fan-out/fan-in, dynamic parallel mapping, retries, recovery, and routing |
| State and knowledge | Scoped memory, file and SQLite persistence, embeddings, retrieval, reranking, grounding, and citations |
| MCP | Server, client, subprocess host, gateway, stdio, HTTP, access policy, audit, and telemetry |
| Operations and governance | Capability decisions, host approvals, audit events, common observability sinks, and module telemetry |
| Guardrails and evaluation | Composable boundary checks, safe buffered output, weighted evaluators, suite metrics, and trajectory regression |
| Learning and adaptation | Experience and reward ledgers, versioned artifacts, offline optimization, regression gates, approvals, activation, and rollback |
| Exploration and discovery | Bounded hypotheses, controlled experiments, evidence review, persistence, and an explicit Learning evidence adapter |
| Controlled execution | Policy-bound Python subprocesses, approvals, resource limits, backend contracts, and audit events |
| Networking | Common HTTP API with cpr/libcurl and cpp-httplib backends |

The complete module map and release boundaries are in the [documentation overview](https://lkimuk.github.io/Wuwe/docs/).

## Build

Requirements:

- CMake 3.25 or newer for the included presets
- Git
- a C++20 compiler
- vcpkg referenced through `VCPKG_ROOT`

Windows:

```powershell
$env:VCPKG_ROOT = "D:\tools\vcpkg"

cmake --preset windows-vcpkg
cmake --build --preset windows-vcpkg-release
ctest --preset windows-vcpkg-release
```

Linux:

```bash
export VCPKG_ROOT="$HOME/vcpkg"

cmake --preset linux-vcpkg
cmake --build --preset linux-vcpkg-release
ctest --preset linux-vcpkg-release
```

macOS Apple Silicon:

```bash
export VCPKG_ROOT=/path/to/vcpkg
cmake --preset macos-arm64-vcpkg
cmake --build --preset macos-arm64-vcpkg-release
ctest --preset macos-arm64-vcpkg-release
```

The official Windows profile uses Schannel and SQLite. Linux and macOS use OpenSSL and SQLite.
Dependencies are restored from the pinned vcpkg manifest into the build tree.

Hardening builds are opt-in and must use a separate build directory. Enable
`WUWE_ENABLE_ADDRESS_SANITIZER` for AddressSanitizer (plus UndefinedBehaviorSanitizer
on supported Clang/GCC toolchains), or `WUWE_ENABLE_THREAD_SANITIZER` for
ThreadSanitizer on supported Clang/GCC toolchains. The two modes are intentionally
mutually exclusive. Concurrency-sensitive tests carry the `concurrency` CTest label:

```bash
cmake -S . -B build-asan -DWUWE_ENABLE_ADDRESS_SANITIZER=ON
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure

cmake -S . -B build-tsan -DWUWE_ENABLE_THREAD_SANITIZER=ON
cmake --build build-tsan
ctest --test-dir build-tsan -L concurrency --repeat until-fail:20 --output-on-failure
```

See [Getting started](https://lkimuk.github.io/Wuwe/docs/getting-started/) and [Dependencies](https://lkimuk.github.io/Wuwe/docs/dependencies/).

For reusable agent behavior, see [Skills](docs/skills.md). Skills assemble declared
instructions and requirements without executing scripts, granting capabilities, or
bypassing Tool Contract, approval, and audit boundaries.

## Minimal client

```cpp
#include <iostream>
#include <wuwe/wuwe.h>

int main() {
  wuwe::llm_config config {
    .model = "gpt-4.1-mini",
  };

  auto client = wuwe::make_llm_client("OpenAI", config);
  const auto response = client->complete("Explain RAII in one paragraph.");

  if (!response) {
    std::cerr << response.error_code.message() << '\n';
    return 1;
  }

  std::cout << response.content << '\n';
}
```

Set `OPENAI_API_KEY` before running the program. Other built-in providers use the same `llm_client` interface.

## Consume an installed SDK

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_agent LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(wuwe 1 CONFIG REQUIRED)

add_executable(my_agent main.cpp)
target_link_libraries(my_agent PRIVATE wuwe::wuwe)
```

Add the Wuwe installation prefix to `CMAKE_PREFIX_PATH` when needed. The exported package requests the public dependencies enabled in that build.

## Package

After building and testing:

```powershell
.\tools\package-wuwe.ps1 -BuildDir build-vcpkg -Configuration Release
```

```bash
bash ./tools/package-wuwe.sh --build-dir build-linux-vcpkg --configuration Release
```

Each archive contains the static SDK, CMake package files, examples, docs, `manifest.json`, checksums, Apache Tika, and a platform-matched Temurin 21 JRE. SQLite and OpenSSL remain public link dependencies when enabled.

See [Packaging](https://lkimuk.github.io/Wuwe/docs/packaging/).

## Production boundaries

- `controlled_process` is a bounded subprocess backend, not a strong sandbox.
- SQLite is intended for local persistence. The SQLite knowledge index uses a C++ linear scan, not ANN search.
- Qdrant and other remote indexes are external services managed by the host.
- The host owns identity, secrets, user consent, approvals, retention, and deployment policy.
- Inspect package metadata and backend enforcement contracts instead of inferring capabilities.

## License

Wuwe is distributed under the terms in [LICENSE](LICENSE).
