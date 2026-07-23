---
id: llm-tools
title: Typed tools
description: Define model-visible tools from C++ aggregates and compose providers.
---

# Typed tools

Wuwe derives a JSON tool schema from a C++ aggregate, parses model arguments into that type, invokes it, and serializes the result.

## Define a tool

```cpp
enum class temperature_unit { celsius, fahrenheit };

struct get_weather {
  static constexpr std::string_view description =
    "Get the current weather for a city.";

  std::string city;
  wuwe::field<temperature_unit> unit {
    .default_value = temperature_unit::celsius,
    .description = "Preferred temperature unit.",
  };

  std::string invoke() const {
    return city + " is 22 degrees.";
  }
};
```

Tool types are aggregates with a static or instance `description` and an `invoke()` method. Supported schema values include strings, booleans, numbers, enums, optionals, vectors, nested aggregates, and `field<T>` metadata.

`tool_field_traits<T, I>` can provide a field description or default when changing the aggregate member type is undesirable.

## Bind tools to a client

```cpp
auto runner = client->bind_tools<get_weather>();
const auto response = runner.complete("What's the weather in Tokyo?");
```

The type name becomes the default tool name. `make_llm_tool<T>()` exposes the generated schema, while `parse_tool_arguments<T>()` and `invoke_reflected_tool<T>()` provide lower-level control.

## Providers

```cpp
auto local = std::make_shared<wuwe::tool_provider<get_weather>>();
auto combined = wuwe::compose_tool_providers(local, another_provider);
```

`tool_provider<T...>` supplies schemas and dispatch for a set of types. `composite_tool_provider` and `compose_tool_providers()` combine providers while preserving each provider's implementation and state.

Context-aware tools can expose `invoke(context)`. Stateful modules such as memory, knowledge, and execution use dedicated provider classes built on the same model-facing tool contract.

## Security boundary

Schema validation is not authorization. A tool that reads files, calls services, changes state, or starts a process should enforce host policy at invocation time. The agent runner can reject calls through `allow_tool_call`, and the execution module adds capability, approval, path, and audit checks for process tools.

See `examples/src/example.cpp` and `examples/src/simple_example.cpp` for built examples.
