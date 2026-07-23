---
id: getting-started
title: Getting started
sidebar_position: 2
description: Configure, build, test, and consume Wuwe 0.1.0.
---

# Getting started

The release presets build Wuwe with C++20, restore pinned dependencies through vcpkg, and enable SQLite. Windows uses Schannel; Linux uses OpenSSL.

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

find_package(wuwe CONFIG REQUIRED)

add_executable(my_agent main.cpp)
target_link_libraries(my_agent PRIVATE wuwe::wuwe)
```

Add the installation prefix to `CMAKE_PREFIX_PATH` if it is not in a standard location. The exported package requests the public dependencies enabled in that Wuwe build.

## Next steps

- [LLM providers](llm-providers.md)
- [Typed tools](llm-tools.md)
- [Reasoning](reasoning.md)
- [Dependencies](dependencies.md)
