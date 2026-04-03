# Wuwe

Wuwe is a C++20 framework for building agents.

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