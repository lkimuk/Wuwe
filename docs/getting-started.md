---
id: getting-started
title: Getting started
sidebar_position: 2
description: Configure, build, test, and consume Wuwe 1.0.0.
---

# Getting started

The release presets build Wuwe with C++20, restore pinned dependencies through vcpkg, and enable SQLite. Windows uses Schannel; Linux and macOS use OpenSSL.

## Requirements

- CMake 3.25 or newer for `CMakePresets.json`
- Git
- a C++20 compiler
- vcpkg at the revision used by the repository baseline

Set `VCPKG_ROOT` to the vcpkg checkout before configuring.

## Windows

```powershell
$env:VCPKG_ROOT = "D:\tools\vcpkg"

cmake --preset windows-vcpkg
cmake --build --preset windows-vcpkg-release
ctest --preset windows-vcpkg-release
```

The preset targets Visual Studio 2022 and `x64-windows`.

## Linux

```bash
export VCPKG_ROOT="$HOME/vcpkg"

cmake --preset linux-vcpkg
cmake --build --preset linux-vcpkg-release
ctest --preset linux-vcpkg-release
```

The certified Linux profile targets Ubuntu 24.04 x64.

## macOS Apple Silicon

Install Apple Command Line Tools, CMake, and vcpkg, then run:

```bash
export VCPKG_ROOT=/path/to/vcpkg
cmake --preset macos-arm64-vcpkg
cmake --build --preset macos-arm64-vcpkg-release
ctest --preset macos-arm64-vcpkg-release
```

The profile targets the repository's `arm64-osx-wuwe` triplet, which fixes the deployment target
at macOS 14. The package includes Tika and a pinned Temurin 21 ARM64 runtime,
so document parsing does not depend on a host Java installation.

## Minimal client

```cpp
#include <iostream>
#include <wuwe/wuwe.h>

int main() {
  wuwe::llm_config config {
    .api_key = wuwe::llm_client_config::load_api_key_from_env(),
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

Set `OPENAI_API_KEY` before running the program. Provider IDs and their environment variables are listed in [LLM providers](llm-providers.md).

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

Add the installation prefix to `CMAKE_PREFIX_PATH` if it is not in a standard location. The exported package requests the public dependencies enabled in that Wuwe build.

Within the 1.x line, the CMake package accepts later 1.x releases as source
compatible. Rebuild consumers after every SDK upgrade; Wuwe does not promise
drop-in C++ ABI compatibility between separately built releases. See
[Versioning and compatibility](versioning.md).

## Consume from source

Wuwe is safe to add as a CMake subdirectory without building its tests,
examples, install rules, or bundled CPR dependency by default. Provide an
existing `cpr::cpr` target, or explicitly allow the pinned dependency fetch:

```cmake
set(WUWE_FETCH_CPR ON CACHE BOOL "")
add_subdirectory(external/wuwe)
target_link_libraries(my_agent PRIVATE wuwe)
```

`WUWE_BUILD_EXAMPLES`, `WUWE_BUILD_TESTS`, and `WUWE_ENABLE_INSTALL` default to
`ON` only for a top-level Wuwe build. `WUWE_ENABLE_STRICT_WARNINGS` is intended
for Wuwe development and CI; it does not add warning flags to installed SDK
consumers.

## Next steps

- [LLM providers](llm-providers.md)
- [Typed tools](llm-tools.md)
- [Reasoning](reasoning.md)
- [Dependencies](dependencies.md)
