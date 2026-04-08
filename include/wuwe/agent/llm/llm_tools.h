#ifndef WUWE_AGENT_LLM_TOOLS_H
#define WUWE_AGENT_LLM_TOOLS_H

#include <array>
#include <concepts>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include <gmp/gmp.hpp>
#include <nlohmann/json.hpp>

#include <wuwe/agent/llm/llm_types.h>
#include <wuwe/common/wuwe_fwd.h>

WUWE_NAMESPACE_BEGIN

struct llm_tool_result {
  std::string content;
  std::error_code error_code;
};

class llm_tool_provider {
public:
  virtual ~llm_tool_provider() = default;

  virtual std::vector<llm_tool> tools() const = 0;
  virtual llm_tool_result invoke(const std::string& name, const std::string& arguments_json) const = 0;
};

namespace detail {

using tool_json = nlohmann::json;

template <typename T>
inline constexpr bool is_optional_v = false;

template <typename T>
inline constexpr bool is_optional_v<std::optional<T>> = true;

template <typename T>
inline constexpr bool is_vector_v = false;

template <typename T, typename Allocator>
inline constexpr bool is_vector_v<std::vector<T, Allocator>> = true;

template <typename T>
concept tool_has_description = requires {
  T::description;
};

template <typename T>
concept tool_has_invoke = requires(const T& value) {
  value.invoke();
};

template <typename T>
std::string json_schema_type_name() {
  using value_type = std::remove_cvref_t<T>;

  if constexpr (std::is_same_v<value_type, bool>) {
    return "boolean";
  }
  else if constexpr (std::is_integral_v<value_type>) {
    return "integer";
  }
  else if constexpr (std::is_floating_point_v<value_type>) {
    return "number";
  }
  else if constexpr (std::is_same_v<value_type, std::string>) {
    return "string";
  }
  else {
    return "object";
  }
}

template <typename T>
tool_json build_json_schema();

template <typename T>
tool_json build_object_json_schema() {
  tool_json properties = tool_json::object();
  tool_json required = tool_json::array();

  constexpr auto member_names = gmp::member_names<T>();

  [&]<std::size_t... Is>(std::index_sequence<Is...>) {
    ((properties[std::string(member_names[Is])] = build_json_schema<gmp::member_type_t<Is, T>>(),
      [&] {
        using member_type = gmp::member_type_t<Is, T>;
        if constexpr (!is_optional_v<member_type>) {
          required.push_back(member_names[Is]);
        }
      }()),
      ...);
  }(std::make_index_sequence<member_names.size()>{});

  return tool_json {
    { "type", "object" },
    { "properties", std::move(properties) },
    { "required", std::move(required) },
    { "additionalProperties", false }
  };
}

template <typename T>
tool_json build_json_schema() {
  using value_type = std::remove_cvref_t<T>;

  if constexpr (is_optional_v<value_type>) {
    return build_json_schema<typename value_type::value_type>();
  }
  else if constexpr (is_vector_v<value_type>) {
    return tool_json {
      { "type", "array" },
      { "items", build_json_schema<typename value_type::value_type>() }
    };
  }
  else if constexpr (std::is_same_v<value_type, std::string>) {
    return tool_json { { "type", "string" } };
  }
  else if constexpr (std::is_same_v<value_type, bool>) {
    return tool_json { { "type", "boolean" } };
  }
  else if constexpr (std::is_integral_v<value_type>) {
    return tool_json { { "type", "integer" } };
  }
  else if constexpr (std::is_floating_point_v<value_type>) {
    return tool_json { { "type", "number" } };
  }
  else if constexpr (std::is_aggregate_v<value_type>) {
    return build_object_json_schema<value_type>();
  }
  else {
    return tool_json { { "type", json_schema_type_name<value_type>() } };
  }
}

template <typename T>
T tool_json_get(const tool_json& json_value) {
  using value_type = std::remove_cvref_t<T>;

  if constexpr (is_optional_v<value_type>) {
    using inner_type = typename value_type::value_type;
    if (json_value.is_null()) {
      return std::nullopt;
    }
    return tool_json_get<inner_type>(json_value);
  }
  else if constexpr (is_vector_v<value_type>) {
    using inner_type = typename value_type::value_type;
    if (!json_value.is_array()) {
      throw tool_json::type_error::create(302, "type must be array", &json_value);
    }
    value_type values;
    values.reserve(json_value.size());
    for (const auto& item : json_value) {
      values.push_back(tool_json_get<inner_type>(item));
    }
    return values;
  }
  else if constexpr (std::is_aggregate_v<value_type> && !std::is_same_v<value_type, std::string>) {
    if (!json_value.is_object()) {
      throw tool_json::type_error::create(302, "type must be object", &json_value);
    }
    return [&]<std::size_t... Is>(std::index_sequence<Is...>) {
      return value_type {
        tool_object_member_get<Is, value_type>(json_value)...
      };
    }(std::make_index_sequence<gmp::member_count<value_type>()>{});
  }
  else {
    return json_value.get<value_type>();
  }
}

template <std::size_t I, typename T>
gmp::member_type_t<I, T> tool_object_member_get(const tool_json& object) {
  using member_type = gmp::member_type_t<I, T>;
  constexpr auto member_names = gmp::member_names<T>();
  const std::string key(member_names[I]);

  if constexpr (is_optional_v<member_type>) {
    const auto it = object.find(key);
    if (it == object.end() || it->is_null()) {
      return std::nullopt;
    }
    return tool_json_get<typename member_type::value_type>(*it);
  }
  else {
    return tool_json_get<member_type>(object.at(key));
  }
}

template <typename T>
std::string tool_result_to_string(T&& value) {
  using value_type = std::remove_cvref_t<T>;

  if constexpr (std::is_same_v<value_type, std::string>) {
    return std::forward<T>(value);
  }
  else if constexpr (std::is_same_v<value_type, std::string_view>) {
    return std::string(value);
  }
  else if constexpr (std::is_same_v<value_type, const char*> || std::is_same_v<value_type, char*>) {
    return value == nullptr ? std::string() : std::string(value);
  }
  else if constexpr (requires { tool_json(std::forward<T>(value)); }) {
    return tool_json(std::forward<T>(value)).dump();
  }
  else {
    std::ostringstream out;
    out << std::forward<T>(value);
    return out.str();
  }
}

template <typename T>
llm_tool_result invoke_reflected_tool(const std::string& arguments_json) {
  static_assert(tool_has_description<T>, "tool must define static constexpr description");
  static_assert(tool_has_invoke<T>, "tool must provide invoke() const");

  try {
    const auto args = tool_json::parse(arguments_json.empty() ? "{}" : arguments_json);
    const auto tool = tool_json_get<T>(args);
    return { .content = tool_result_to_string(tool.invoke()) };
  }
  catch (const std::exception& ex) {
    return {
      .content = ex.what(),
      .error_code = std::make_error_code(std::errc::invalid_argument)
    };
  }
}

template <typename T>
llm_tool make_reflected_tool() {
  static_assert(tool_has_description<T>, "tool must define static constexpr description");
  static_assert(tool_has_invoke<T>, "tool must provide invoke() const");
  static_assert(std::is_aggregate_v<T>, "tool must be an aggregate so GMP can reflect its fields");

  return llm_tool {
    .name = std::string(gmp::type_name<T>().to_string_view()),
    .description = std::string(T::description),
    .parameters_json_schema = build_object_json_schema<T>().dump()
  };
}

template <typename T>
bool try_invoke_tool(
  const std::string& expected_name, const std::string& name, const std::string& arguments_json,
  llm_tool_result& result) {
  if (expected_name != name) {
    return false;
  }
  result = invoke_reflected_tool<T>(arguments_json);
  return true;
}

} // namespace detail

template <typename... Tools>
class llm_reflected_tool_provider final : public llm_tool_provider {
public:
  std::vector<llm_tool> tools() const override {
    return { detail::make_reflected_tool<Tools>()... };
  }

  llm_tool_result invoke(const std::string& name, const std::string& arguments_json) const override {
    llm_tool_result result {
      .content = "tool not found: " + name,
      .error_code = std::make_error_code(std::errc::function_not_supported)
    };

    (detail::try_invoke_tool<Tools>(
       std::string(gmp::type_name<Tools>().to_string_view()), name, arguments_json, result)
     || ...);
    return result;
  }
};

template <>
class llm_reflected_tool_provider<> final : public llm_tool_provider {
public:
  std::vector<llm_tool> tools() const override {
    return {};
  }

  llm_tool_result invoke(const std::string& name, const std::string&) const override {
    return {
      .content = "tool not found: " + name,
      .error_code = std::make_error_code(std::errc::function_not_supported)
    };
  }
};

WUWE_NAMESPACE_END

#endif // WUWE_AGENT_LLM_TOOLS_H
