# LLM Tools

This document describes how reflected LLM tools are defined in Wuwe.

## Overview

LLM tools are ordinary C++ aggregate types with:

- either `static constexpr std::string_view description` or a default-initialized instance member
  named `description`
- public fields used as tool parameters
- an `invoke() const` method used to execute the tool when using the default stateless
  `tool_provider<Tools...>`

Example:

```cpp
struct get_weather {
  static constexpr std::string_view description =
    "Get the current weather in a given location.";

  std::string city;

  std::string invoke() const {
    return "sunny";
  }
};
```

The tool can then be bound with:

```cpp
auto client = factory.create_shared("OpenRouter", config);
auto runner = client->build_tools<get_weather>();
```

## Description Styles

Two description styles are supported.

### 1. `static constexpr description`

```cpp
struct get_weather {
  static constexpr std::string_view description =
    "Get the current weather in a given location.";

  std::string city;
};
```

### 2. Instance `description`

```cpp
struct get_weather {
  std::string_view description = "Get the current weather in a given location.";

  std::string city;
};
```

When using the instance form, the type must be default-initializable because Wuwe reads the
description from `T{}.description`.

### Description precedence

If both description forms are present, Wuwe uses this priority:

1. `static constexpr description`
2. instance `description`

The instance `description` member is treated as tool metadata, not as a tool parameter.

## Supported Parameter Styles

There are two supported ways to define tool parameter metadata.

### 1. Raw fields

Use normal C++ fields when you only need the parameter structure, or when you prefer to provide
metadata separately.

```cpp
struct get_weather {
  static constexpr std::string_view description = "...";

  std::string city;
  temperature_unit unit { temperature_unit::celsius };
};
```

Optional metadata for raw fields can be supplied through `llm_tool_field_traits<Tool, I>`, where
`I` is the zero-based member index.

```cpp
template <>
struct wuwe::llm_tool_field_traits<get_weather, 0> {
  static constexpr std::string_view description =
    "Target city, for example New York or Tokyo.";
};
```

### 2. `field<T>`

Use `wuwe::field<T>` when you want metadata to live next to the field itself.

```cpp
struct get_weather {
  static constexpr std::string_view description = "...";

  std::string city;
  wuwe::field<temperature_unit> unit {
    .default_value = temperature_unit::celsius,
    .description = "Preferred unit for the reported temperature."
  };
};
```

`field<T>` stores:

- `value`
- `default_value`
- `description`

and can still be read naturally inside `invoke()`.

## Metadata Precedence Rule

The metadata rule is intentionally simple:

- If a member is `field<T>`, metadata is read only from `field<T>` itself.
- If a member is a raw field, metadata is read from `llm_tool_field_traits<Tool, I>`.

Do not define both for the same member. `field<T>` is the self-contained form, while
`llm_tool_field_traits` is the fallback for raw fields.

## Schema Generation

Tool schemas are generated automatically from reflection.

The following are supported:

- `std::string`
- `bool`
- integral and floating-point types
- enums
- `std::optional<T>`
- `std::vector<T>`
- nested aggregate types
- `field<T>`

For enums, schema is emitted as a string enum when reflection can enumerate the values.

For `field<T>`, schema metadata such as `description` and `default` comes from the field object.

For raw fields, schema metadata comes from `llm_tool_field_traits`.

## Argument Parsing

Arguments returned by the model are parsed back into the tool type automatically.

Behavior:

- unknown object fields are rejected
- missing required fields are rejected
- optional fields may be omitted
- raw fields may use `llm_tool_field_traits` defaults
- `field<T>` may use `field<T>::default_value`
- enum arguments may be passed by name

## Return Values

`invoke()` may return:

- `std::string`
- `std::string_view`
- C strings
- enums
- `std::optional<T>`
- `std::vector<T>`
- aggregate types
- JSON-compatible scalar values

Structured return values are serialized to JSON automatically.

Example:

```cpp
struct weather_report {
  std::string city;
  std::string summary;
  int high;
  temperature_unit unit;
  std::optional<std::string> advisory;
};
```

Returning `weather_report` from `invoke()` produces a JSON object for the model.

## Stateful Reflected Tools

The default `tool_provider<Tools...>` is designed for stateless tools:

```text
JSON arguments -> reflected Tool object -> invoke()
```

Some tools need application-owned runtime state that must not be supplied by the model. Examples
include database handles, user/session scope, review callbacks, auth state, or memory contexts.
Those values should not appear in the JSON schema and should not be model-fillable fields.

For these tools, Wuwe supports reusing the same reflected aggregate parameter style while injecting
runtime state from a custom provider:

```text
JSON arguments -> reflected Tool object -> invoke(context)
provider-owned state ------------------------^
```

The tool argument type can still be an aggregate with `description` and public model-fillable
fields:

```cpp
struct save_memory {
  static constexpr std::string_view description =
    "Save a durable long-term memory record.";

  std::string content;
  std::optional<std::string> topic;

  llm_tool_result invoke(const memory_tool_context& context) const;
};
```

The custom provider is responsible for:

- generating the schema from the reflected argument type
- parsing JSON arguments into the reflected argument type
- holding private application state
- calling `tool.invoke(context)`

This keeps the model-facing parameter style consistent with ordinary reflected tools while keeping
private state outside the schema.

The built-in memory tools use this pattern through `memory_tool_provider`: `save_memory` and
`search_memory` are reflected aggregate argument types, while `memory_context`, `memory_scope`,
policy limits, and review callbacks are provider state.

### Why Not `injected<T>` Fields Yet?

Another possible design is an injected field marker:

```cpp
template<typename T>
struct injected {
  T value;
};

struct save_memory {
  static constexpr std::string_view description =
    "Save a durable long-term memory record.";

  std::string content;
  injected<memory_context*> memory;
  injected<memory_scope> scope;

  llm_tool_result invoke() const;
};
```

This style is elegant because the tool keeps a no-argument `invoke()` and looks closer to ordinary
stateless tools. However, it requires deeper support in the reflection system:

- schema generation must skip `injected<T>`
- JSON parsing must not expect injected fields
- required-field checks must ignore injected fields
- nested aggregates need clear rules for whether injected fields are allowed
- the provider needs a general mapping from requested injected types to runtime objects
- missing or ambiguous injected values need consistent errors

For memory tools, Wuwe currently chooses `invoke(context)` instead:

```text
reflected model arguments + provider-owned context -> invoke(context)
```

The trade-off is:

| Dimension | `invoke(context)` | `injected<T>` |
|---|---|---|
| Accuracy | Higher: model fields and private state are clearly separated | Good, but depends on every reflection path skipping injected fields correctly |
| Elegance | Good, but context is explicit | Higher: tools can keep no-argument `invoke()` |
| Generability | Higher: each provider defines one context type | Lower initially: needs generic injection rules |
| Usability | Higher for safety-sensitive tools because state ownership is explicit | Good, but can feel magical without strong diagnostics |
| Simplicity | Higher: minimal changes to existing tools | Lower: touches schema, parsing, defaults, and field filtering |

The intended direction is:

- Use `invoke(context)` for the first generation of stateful tools.
- Consider `injected<T>` only after several stateful providers share the same injection needs.
- If `injected<T>` is added later, it should be a general Tool-system feature, not a memory-only
  special case.

## Multi-tool Usage

Multiple tools can be registered together:

```cpp
auto runner = client->build_tools<get_weather, get_happy_fact>();
```

If the model calls an unknown tool, the runtime returns an error message that includes the list of
available tool names.

## Choosing Between Raw Fields and `field<T>`

Use raw fields when:

- you want the simplest possible data structure
- you do not need extra metadata
- you are comfortable adding metadata externally through traits

Use `field<T>` when:

- you want metadata declared next to the field
- you want clearer ownership of defaults and descriptions
- you want a style closer to schema-first tool definitions
