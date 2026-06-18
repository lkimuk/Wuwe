#include <wuwe/agent/execution/execution_tools.hpp>

#include <chrono>
#include <exception>
#include <map>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include <wuwe/agent/execution/execution_codec.hpp>

namespace wuwe::agent::execution {
namespace {

std::optional<int> max_timeout_from_policy(
  const execution_policy& policy,
  const execution_tool_options& options) {
  if (options.max_timeout_ms.has_value()) {
    return options.max_timeout_ms;
  }
  if (policy.max_limits.timeout.count() > 0) {
    return static_cast<int>(policy.max_limits.timeout.count());
  }
  return std::nullopt;
}

std::optional<int> default_timeout_from_policy(
  const execution_policy& policy,
  const execution_tool_options& options) {
  if (options.default_timeout_ms.has_value()) {
    return options.default_timeout_ms;
  }
  if (policy.max_limits.timeout.count() > 0) {
    return static_cast<int>(policy.max_limits.timeout.count());
  }
  return std::nullopt;
}

nlohmann::json build_execution_tool_schema(
  const execution_policy& policy,
  const execution_tool_options& options) {
  nlohmann::json code_schema {
    { "type", "string" },
    { "description", "Short Python code snippet to execute." },
  };
  if (policy.max_limits.max_code_bytes > 0) {
    code_schema["maxLength"] = policy.max_limits.max_code_bytes;
  }

  nlohmann::json stdin_schema {
    { "type", "string" },
    { "description", "Optional stdin text supplied to the Python snippet." },
  };
  if (policy.max_limits.max_stdin_bytes > 0) {
    stdin_schema["maxLength"] = policy.max_limits.max_stdin_bytes;
  }
  if (!options.allow_empty_stdin) {
    stdin_schema["minLength"] = 1;
  }

  nlohmann::json timeout_schema {
    { "type", "integer" },
    { "description", "Optional wall-clock timeout in milliseconds." },
  };
  if (options.min_timeout_ms.has_value()) {
    timeout_schema["minimum"] = *options.min_timeout_ms;
  }
  if (const auto max_timeout = max_timeout_from_policy(policy, options)) {
    timeout_schema["maximum"] = *max_timeout;
  }
  if (const auto default_timeout = default_timeout_from_policy(policy, options)) {
    timeout_schema["default"] = *default_timeout;
  }

  return nlohmann::json {
    { "type", "object" },
    { "properties",
      {
        { "code", std::move(code_schema) },
        { "stdin_text", std::move(stdin_schema) },
        { "timeout_ms", std::move(timeout_schema) },
      } },
    { "required", { "code" } },
    { "additionalProperties", options.allow_additional_arguments },
  };
}

llm_tool_result rejected_tool_result(
  execution_runtime& runtime,
  const execution_tool_options& options,
  std::string event_name,
  std::string message,
  std::map<std::string, std::string> metadata) {
  runtime.audit_tool_rejection(event_name, options.tool_name, message, metadata);

  metadata.try_emplace("denial_kind", std::move(event_name));
  execution_result result {
    .termination_reason = execution_termination_reason::policy_denied,
    .error_message = std::move(message),
    .metadata = std::move(metadata),
  };
  return {
    .content = execution_result_to_json(result).dump(),
    .error_code = std::make_error_code(std::errc::operation_not_permitted),
  };
}

std::map<std::string, std::string> base_argument_metadata(
  const std::string& arguments_json) {
  return {
    { "arguments_bytes", std::to_string(arguments_json.size()) },
  };
}

} // namespace

execution_tool_provider::execution_tool_provider(
  execution_runtime& runtime,
  execution_tool_options options)
    : runtime_(runtime), options_(std::move(options)) {
}

std::vector<llm_tool> execution_tool_provider::tools() const {
  llm_tool tool {
    .name = options_.tool_name,
    .description = options_.description,
    .parameters_json_schema =
      build_execution_tool_schema(runtime_.policy(), options_).dump(),
  };
  return { std::move(tool) };
}

llm_tool_result execution_tool_provider::invoke(
  const std::string& name,
  const std::string& arguments_json) const {
  return invoke(name, arguments_json, {});
}

llm_tool_result execution_tool_provider::invoke(
  const std::string& name,
  const std::string& arguments_json,
  std::stop_token stop_token) const {
  if (name != options_.tool_name) {
    return {
      .content = "tool not found: " + name,
      .error_code = std::make_error_code(std::errc::function_not_supported),
    };
  }

  if (options_.max_arguments_bytes > 0 &&
      arguments_json.size() > options_.max_arguments_bytes) {
    auto metadata = base_argument_metadata(arguments_json);
    metadata["max_arguments_bytes"] =
      std::to_string(options_.max_arguments_bytes);
    return rejected_tool_result(
      runtime_,
      options_,
      "arguments_limit",
      "Execution denied: tool arguments are too large. arguments_bytes=" +
        std::to_string(arguments_json.size()) +
        " max_arguments_bytes=" +
        std::to_string(options_.max_arguments_bytes) + ".",
      std::move(metadata));
  }

  try {
    auto args = parse_tool_arguments<run_python_snippet>(
      std::string_view(arguments_json));

    if (!options_.allow_empty_stdin && args.stdin_text.has_value() &&
        args.stdin_text->empty()) {
      auto metadata = base_argument_metadata(arguments_json);
      metadata["field"] = "stdin_text";
      return rejected_tool_result(
        runtime_,
        options_,
        "schema_invalid",
        "Execution denied: stdin_text must not be empty.",
        std::move(metadata));
    }

    if (args.timeout_ms.has_value()) {
      if (options_.min_timeout_ms.has_value() &&
          *args.timeout_ms < *options_.min_timeout_ms) {
        auto metadata = base_argument_metadata(arguments_json);
        metadata["timeout_ms"] = std::to_string(*args.timeout_ms);
        metadata["min_timeout_ms"] = std::to_string(*options_.min_timeout_ms);
        return rejected_tool_result(
          runtime_,
          options_,
          "timeout_limit",
          "Execution denied: timeout_ms is below the minimum.",
          std::move(metadata));
      }

      if (const auto max_timeout = max_timeout_from_policy(runtime_.policy(), options_);
          max_timeout.has_value() && *args.timeout_ms > *max_timeout &&
          options_.reject_timeout_outside_limits) {
        auto metadata = base_argument_metadata(arguments_json);
        metadata["timeout_ms"] = std::to_string(*args.timeout_ms);
        metadata["max_timeout_ms"] = std::to_string(*max_timeout);
        return rejected_tool_result(
          runtime_,
          options_,
          "timeout_limit",
          "Execution denied: timeout_ms exceeds the maximum.",
          std::move(metadata));
      }
    }

    execution_request request;
    request.language = execution_language::python;
    request.code = std::move(args.code);
    if (args.stdin_text.has_value()) {
      request.stdin_text = std::move(*args.stdin_text);
    }
    if (args.timeout_ms.has_value()) {
      request.limits.timeout = std::chrono::milliseconds(*args.timeout_ms);
    }
    request.metadata["tool_name"] = options_.tool_name;

    const auto result = runtime_.run(std::move(request), stop_token);
    return {
      .content = execution_result_to_json(result).dump(),
      .error_code =
        result.termination_reason == execution_termination_reason::exited
          ? std::error_code {}
          : std::make_error_code(std::errc::operation_not_permitted),
    };
  }
  catch (const std::exception& ex) {
    auto metadata = base_argument_metadata(arguments_json);
    metadata["parse_error"] = ex.what();
    return rejected_tool_result(
      runtime_,
      options_,
      "schema_invalid",
      "invalid arguments for tool '" + options_.tool_name + "': " + ex.what(),
      std::move(metadata));
  }
}

} // namespace wuwe::agent::execution
