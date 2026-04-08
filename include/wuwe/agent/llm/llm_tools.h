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
#include <tuple>
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

template <typename T>
struct field {
  using value_type = T;

  T value {};
  std::optional<T> default_value {};
  std::string_view description {};

  constexpr operator T&() noexcept {
    return value;
  }

  constexpr operator const T&() const noexcept {
    return value;
  }

  constexpr T* operator->() noexcept {
    return &value;
  }

  constexpr const T* operator->() const noexcept {
    return &value;
  }
};

class llm_tool_provider {
public:
  virtual ~llm_tool_provider() = default;

  virtual std::vector<llm_tool> tools() const = 0;
  virtual llm_tool_result invoke(const std::string& name, const std::string& arguments_json) const = 0;
};

template <typename Tool, std::size_t I>
struct llm_tool_field_traits {};

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
inline constexpr bool is_field_v = false;

template <typename T>
inline constexpr bool is_field_v<field<T>> = true;

template <typename T>
struct unwrap_field {
  using type = T;
};

template <typename T>
struct unwrap_field<field<T>> {
  using type = T;
};

template <typename T>
using unwrap_field_t = typename unwrap_field<T>::type;

template <typename T>
concept tool_has_description = requires {
  T::description;
};

template <typename T>
concept tool_has_invoke = requires(const T& value) {
  value.invoke();
};

template <typename Tool, std::size_t I>
concept field_has_description = requires {
  llm_tool_field_traits<Tool, I>::description;
};

template <typename Tool, std::size_t I>
concept field_has_default_value = requires {
  llm_tool_field_traits<Tool, I>::default_value();
};

template <typename T, bool = std::is_enum_v<T>>
struct reflectable_enum_traits : std::false_type {};

template <typename T>
struct reflectable_enum_traits<T, true> : std::bool_constant<(gmp::enum_count<T>() > 0)> {};

template <typename T>
inline constexpr bool is_reflectable_enum_v = reflectable_enum_traits<T>::value;

#define WUWE_LLM_FOR_EACH_MEMBER_IMPL(N) \
  template <typename T, typename F> \
  void for_each_member_impl(T&& value, F&& f, gmp::constant_arg_t<N>) { \
    auto&& [GMP_GET_FIRST_N(N, GMP_IDENTIFIERS)] = value; \
    auto members = std::forward_as_tuple(GMP_GET_FIRST_N(N, GMP_IDENTIFIERS)); \
    std::size_t index = 0; \
    std::apply( \
      [&](auto&&... member) { (f(std::forward<decltype(member)>(member), index++), ...); }, members); \
  }

GMP_FOR_EACH(WUWE_LLM_FOR_EACH_MEMBER_IMPL, GMP_RANGE(1, GMP_INC(GMP_MAX_SUPPORTED_FIELDS)))

#undef WUWE_LLM_FOR_EACH_MEMBER_IMPL

template <typename T, typename F>
void for_each_member(T&&, F&&, gmp::constant_arg_t<0>) {}

template <typename T, typename F>
void for_each_member(T&& value, F&& f) {
  constexpr auto member_count = gmp::member_count<std::remove_cvref_t<T>>();
  for_each_member_impl(std::forward<T>(value), std::forward<F>(f), gmp::constant_arg<member_count>);
}

template <typename T>
std::string type_name_string() {
  return std::string(gmp::type_name<T>().to_string_view());
}

template <typename T>
tool_json build_json_value(T&& value);

template <std::size_t I, typename T>
decltype(auto) get_member_ref(T&& value);

template <typename Enum>
Enum parse_reflectable_enum(const tool_json& json_value) {
  if (json_value.is_string()) {
    const auto enum_name = json_value.get<std::string>();
    constexpr auto enum_names = gmp::enum_names<Enum>();
    for (std::size_t i = 0; i < enum_names.size(); ++i) {
      if (enum_names[i] == enum_name) {
        return static_cast<Enum>(i);
      }
    }

    std::ostringstream message;
    message << "invalid enum value '" << enum_name << "', expected one of: ";
    for (std::size_t i = 0; i < enum_names.size(); ++i) {
      if (i != 0) {
        message << ", ";
      }
      message << enum_names[i];
    }
    throw std::invalid_argument(message.str());
  }

  return static_cast<Enum>(json_value.get<std::underlying_type_t<Enum>>());
}

template <typename Enum>
std::string reflectable_enum_to_string(Enum value) {
  constexpr auto enum_names = gmp::enum_names<Enum>();
  const auto index = static_cast<std::size_t>(value);
  if (index < enum_names.size()) {
    return std::string(enum_names[index]);
  }
  return std::to_string(static_cast<std::underlying_type_t<Enum>>(value));
}

template <typename T>
tool_json build_json_schema();

template <typename T>
void validate_object_keys(const tool_json& json_value) {
  constexpr auto member_names = gmp::member_names<T>();

  for (const auto& [key, _] : json_value.items()) {
    bool found = false;
    for (const auto member_name : member_names) {
      if (member_name == key) {
        found = true;
        break;
      }
    }

    if (!found) {
      std::ostringstream message;
      message << "unexpected field '" << key << "'";
      if constexpr (member_names.size() > 0) {
        message << ", expected fields: ";
        for (std::size_t i = 0; i < member_names.size(); ++i) {
          if (i != 0) {
            message << ", ";
          }
          message << member_names[i];
        }
      }
      throw std::invalid_argument(message.str());
    }
  }
}

template <typename T>
tool_json build_object_json_schema() {
  tool_json properties = tool_json::object();
  tool_json required = tool_json::array();

  constexpr auto member_names = gmp::member_names<T>();
  constexpr bool has_default_object = std::default_initializable<T>;
  const auto default_object = []() -> std::optional<T> {
    if constexpr (has_default_object) {
      return T {};
    }
    else {
      return std::nullopt;
    }
  }();

  [&]<std::size_t... Is>(std::index_sequence<Is...>) {
    (([&] {
        using member_type = gmp::member_type_t<Is, T>;
        using schema_type = unwrap_field_t<member_type>;
        auto field_schema = build_json_schema<schema_type>();

        if constexpr (is_field_v<member_type>) {
          if constexpr (has_default_object) {
            const auto& member = get_member_ref<Is>(*default_object);
            if (!member.description.empty()) {
              field_schema["description"] = member.description;
            }
            if (member.default_value.has_value()) {
              field_schema["default"] = build_json_value(*member.default_value);
            }
          }
        }
        else if constexpr (field_has_description<T, Is>) {
          field_schema["description"] = llm_tool_field_traits<T, Is>::description;
        }

        if constexpr (!is_field_v<member_type> && field_has_default_value<T, Is>) {
          field_schema["default"] = build_json_value(llm_tool_field_traits<T, Is>::default_value());
        }

        properties[std::string(member_names[Is])] = std::move(field_schema);

        if constexpr (is_field_v<member_type>) {
          if constexpr (has_default_object) {
            const auto& member = get_member_ref<Is>(*default_object);
            if (!member.default_value.has_value() && !is_optional_v<schema_type>) {
              required.push_back(member_names[Is]);
            }
          }
          else if constexpr (!is_optional_v<schema_type>) {
            required.push_back(member_names[Is]);
          }
        }
        else if constexpr (!is_optional_v<member_type> && !field_has_default_value<T, Is>) {
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

  if constexpr (is_field_v<value_type>) {
    return build_json_schema<typename value_type::value_type>();
  }
  else if constexpr (is_optional_v<value_type>) {
    return build_json_schema<typename value_type::value_type>();
  }
  else if constexpr (is_vector_v<value_type>) {
    return tool_json {
      { "type", "array" },
      { "items", build_json_schema<typename value_type::value_type>() }
    };
  }
  else if constexpr (is_reflectable_enum_v<value_type>) {
    auto enum_values = tool_json::array();
    constexpr auto enum_names = gmp::enum_names<value_type>();
    for (const auto enum_name : enum_names) {
      enum_values.push_back(enum_name);
    }
    return tool_json {
      { "type", "string" },
      { "enum", std::move(enum_values) }
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
    return tool_json { { "type", "object" } };
  }
}

template <typename T>
T tool_json_get(const tool_json& json_value) {
  using value_type = std::remove_cvref_t<T>;

  if constexpr (is_field_v<value_type>) {
    value_type result {};
    result.value = tool_json_get<typename value_type::value_type>(json_value);
    return result;
  }
  else if constexpr (is_optional_v<value_type>) {
    using inner_type = typename value_type::value_type;
    if (json_value.is_null()) {
      return std::nullopt;
    }
    return tool_json_get<inner_type>(json_value);
  }
  else if constexpr (is_vector_v<value_type>) {
    using inner_type = typename value_type::value_type;
    if (!json_value.is_array()) {
      throw std::invalid_argument("type must be array");
    }
    value_type values;
    values.reserve(json_value.size());
    for (const auto& item : json_value) {
      values.push_back(tool_json_get<inner_type>(item));
    }
    return values;
  }
  else if constexpr (is_reflectable_enum_v<value_type>) {
    return parse_reflectable_enum<value_type>(json_value);
  }
  else if constexpr (std::is_aggregate_v<value_type> && !std::is_same_v<value_type, std::string>) {
    if (!json_value.is_object()) {
      throw std::invalid_argument("type must be object");
    }
    validate_object_keys<value_type>(json_value);
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
  using schema_type = unwrap_field_t<member_type>;
  constexpr auto member_names = gmp::member_names<T>();
  const std::string key(member_names[I]);
  const auto it = object.find(key);
  const auto prototype = []() -> std::optional<T> {
    if constexpr (std::default_initializable<T>) {
      return T {};
    }
    else {
      return std::nullopt;
    }
  }();

  if constexpr (!is_field_v<member_type> && field_has_default_value<T, I>) {
    if (it == object.end() || it->is_null()) {
      return llm_tool_field_traits<T, I>::default_value();
    }
  }

  if constexpr (is_field_v<member_type>) {
    member_type result {};

    if constexpr (std::default_initializable<T>) {
      const auto& member = get_member_ref<I>(*prototype);
      result.description = member.description;
      result.default_value = member.default_value;
    }

    if (it == object.end() || it->is_null()) {
      if (result.default_value.has_value()) {
        result.value = *result.default_value;
        return result;
      }
      if constexpr (is_optional_v<schema_type>) {
        result.value = std::nullopt;
        return result;
      }

      std::ostringstream message;
      message << "missing required field '" << key << "'";
      throw std::invalid_argument(message.str());
    }

    result.value = tool_json_get<schema_type>(*it);
    return result;
  }
  else if constexpr (is_optional_v<member_type>) {
    if (it == object.end() || it->is_null()) {
      return std::nullopt;
    }
    return tool_json_get<typename member_type::value_type>(*it);
  }
  else {
    if (it == object.end()) {
      std::ostringstream message;
      message << "missing required field '" << key << "'";
      throw std::invalid_argument(message.str());
    }
    return tool_json_get<member_type>(*it);
  }
}

template <typename T>
tool_json build_json_value(T&& value) {
  using value_type = std::remove_cvref_t<T>;

  if constexpr (is_field_v<value_type>) {
    return build_json_value(value.value);
  }
  else if constexpr (is_optional_v<value_type>) {
    if (!value.has_value()) {
      return nullptr;
    }
    return build_json_value(*std::forward<T>(value));
  }
  else if constexpr (is_vector_v<value_type>) {
    auto result = tool_json::array();
    for (auto&& item : value) {
      result.push_back(build_json_value(item));
    }
    return result;
  }
  else if constexpr (is_reflectable_enum_v<value_type>) {
    return reflectable_enum_to_string(value);
  }
  else if constexpr (std::is_enum_v<value_type>) {
    return static_cast<std::underlying_type_t<value_type>>(value);
  }
  else if constexpr (std::is_same_v<value_type, std::string>) {
    return value;
  }
  else if constexpr (std::is_same_v<value_type, std::string_view>) {
    return std::string(value);
  }
  else if constexpr (std::is_same_v<value_type, const char*> || std::is_same_v<value_type, char*>) {
    return value == nullptr ? tool_json(nullptr) : tool_json(value);
  }
  else if constexpr (
    std::is_same_v<value_type, bool> || std::is_integral_v<value_type> ||
    std::is_floating_point_v<value_type>) {
    return value;
  }
  else if constexpr (std::is_aggregate_v<value_type>) {
    tool_json result = tool_json::object();
    constexpr auto member_names = gmp::member_names<value_type>();
    for_each_member(std::forward<T>(value), [&](auto&& member, std::size_t index) {
      result[std::string(member_names[index])] = build_json_value(std::forward<decltype(member)>(member));
    });
    return result;
  }
  else if constexpr (requires { tool_json(std::forward<T>(value)); }) {
    return tool_json(std::forward<T>(value));
  }
  else {
    std::ostringstream out;
    out << std::forward<T>(value);
    return out.str();
  }
}

inline std::string dump_json_compact(const tool_json& json_value) {
  return json_value.dump(-1, ' ', false, tool_json::error_handler_t::replace);
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
  else {
    return dump_json_compact(build_json_value(std::forward<T>(value)));
  }
}

template <typename... Tools>
std::string available_tool_names() {
  std::ostringstream out;
  std::size_t index = 0;
  ((out << (index++ == 0 ? "" : ", ") << type_name_string<Tools>()), ...);
  return out.str();
}

#define WUWE_LLM_GET_MEMBER_REF_IMPL(N) \
  template <std::size_t I, typename T> \
  decltype(auto) get_member_ref_impl(T&& value, gmp::constant_arg_t<N>) { \
    auto&& [GMP_GET_FIRST_N(N, GMP_IDENTIFIERS)] = value; \
    return std::get<I>(std::forward_as_tuple(GMP_GET_FIRST_N(N, GMP_IDENTIFIERS))); \
  }

GMP_FOR_EACH(WUWE_LLM_GET_MEMBER_REF_IMPL, GMP_RANGE(1, GMP_INC(GMP_MAX_SUPPORTED_FIELDS)))

#undef WUWE_LLM_GET_MEMBER_REF_IMPL

template <std::size_t I, typename T>
decltype(auto) get_member_ref(T&& value) {
  constexpr auto member_count = gmp::member_count<std::remove_cvref_t<T>>();
  static_assert(I < member_count, "member index out of range");
  return get_member_ref_impl<I>(std::forward<T>(value), gmp::constant_arg<member_count>);
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
      .content =
        "invalid arguments for tool '" + type_name_string<T>() + "': " + ex.what(),
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
    .name = type_name_string<T>(),
    .description = std::string(T::description),
    .parameters_json_schema = dump_json_compact(build_object_json_schema<T>())
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
      .content = "tool not found: " + name + ". Available tools: " + detail::available_tool_names<Tools...>(),
      .error_code = std::make_error_code(std::errc::function_not_supported)
    };

    (detail::try_invoke_tool<Tools>(detail::type_name_string<Tools>(), name, arguments_json, result) || ...);
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
