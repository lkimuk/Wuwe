#ifndef WUWE_AGENT_LLM_LLM_CAPABILITIES_HPP
#define WUWE_AGENT_LLM_LLM_CAPABILITIES_HPP

#include <cmath>
#include <optional>
#include <string>
#include <unordered_set>

#include <wuwe/agent/llm/llm_error.h>
#include <wuwe/agent/llm/llm_types.h>

namespace wuwe::agent::llm {

struct llm_request_validation {
  std::error_code error_code;
  std::string message;
  std::string capability;

  [[nodiscard]] explicit operator bool() const noexcept {
    return !error_code;
  }
};

[[nodiscard]] inline llm_request_validation validate_llm_request(
  const ::wuwe::llm_request& request, const ::wuwe::llm_provider_capabilities& capabilities) {
  if (!std::isfinite(request.temperature) || request.temperature < 0.0) {
    return {
      .error_code = make_error_code(llm_error_code::invalid_request),
      .message = "LLM request temperature must be finite and non-negative",
    };
  }
  if (request.max_output_tokens && *request.max_output_tokens <= 0) {
    return {
      .error_code = make_error_code(llm_error_code::invalid_request),
      .message = "LLM request max_output_tokens must be positive",
    };
  }
  std::unordered_set<std::string> tool_names;
  for (const auto& message : request.messages) {
    if (message.provider_state &&
        (message.role != "assistant" || message.provider_state->provider.empty() ||
          message.provider_state->data.empty())) {
      return { .error_code = make_error_code(llm_error_code::invalid_request),
        .message =
          "Opaque continuation state requires an assistant message and provider identity" };
    }
    if (message.provider_state && capabilities.declared && !capabilities.provider_state) {
      return { .error_code = make_error_code(llm_error_code::unsupported_capability),
        .message = "LLM provider does not accept opaque continuation state",
        .capability = "provider_state" };
    }
  }
  for (const auto& tool : request.tools) {
    if (tool.name.empty()) {
      return {
        .error_code = make_error_code(llm_error_code::invalid_request),
        .message = "LLM tool name must not be empty",
      };
    }
    if (!tool_names.insert(tool.name).second) {
      return {
        .error_code = make_error_code(llm_error_code::invalid_request),
        .message = "LLM tool names must be unique",
      };
    }
    const auto schema = nlohmann::json::parse(tool.parameters_json_schema, nullptr, false);
    if (schema.is_discarded() || !schema.is_object()) {
      return {
        .error_code = make_error_code(llm_error_code::invalid_request),
        .message = "LLM tool parameters schema must be a JSON object",
      };
    }
  }
  if (capabilities.declared && !request.tools.empty() && !capabilities.tools) {
    return {
      .error_code = make_error_code(llm_error_code::unsupported_capability),
      .message = "LLM provider does not support tools",
      .capability = "tools",
    };
  }
  if (request.tool_choice) {
    if (request.tool_choice->mode < ::wuwe::llm_tool_choice_mode::auto_ ||
        request.tool_choice->mode > ::wuwe::llm_tool_choice_mode::named) {
      return {
        .error_code = make_error_code(llm_error_code::invalid_request),
        .message = "LLM tool choice mode is invalid",
      };
    }
    if (request.tool_choice->mode != ::wuwe::llm_tool_choice_mode::named &&
        !request.tool_choice->name.empty()) {
      return {
        .error_code = make_error_code(llm_error_code::invalid_request),
        .message = "only named tool choice may specify a tool name",
      };
    }
    if (capabilities.declared && !capabilities.tool_choice) {
      return {
        .error_code = make_error_code(llm_error_code::unsupported_capability),
        .message = "LLM provider does not support tool choice",
        .capability = "tool_choice",
      };
    }
    if ((request.tool_choice->mode == ::wuwe::llm_tool_choice_mode::required ||
          request.tool_choice->mode == ::wuwe::llm_tool_choice_mode::named) &&
        request.tools.empty()) {
      return {
        .error_code = make_error_code(llm_error_code::invalid_request),
        .message = "required or named tool choice requires at least one tool",
      };
    }
    if (request.tool_choice->mode == ::wuwe::llm_tool_choice_mode::named &&
        (request.tool_choice->name.empty() || !tool_names.contains(request.tool_choice->name))) {
      return {
        .error_code = make_error_code(llm_error_code::invalid_request),
        .message = "named tool choice must reference a declared tool",
      };
    }
  }
  if (request.response_format) {
    if (*request.response_format != "json_object") {
      return {
        .error_code = make_error_code(llm_error_code::invalid_request),
        .message = "unsupported legacy LLM response format",
      };
    }
    if (capabilities.declared && !capabilities.json_response_format) {
      return {
        .error_code = make_error_code(llm_error_code::unsupported_capability),
        .message = "LLM provider does not support JSON response format",
        .capability = "json_response_format",
      };
    }
  }
  for (const auto& stop : request.stop_sequences) {
    if (stop.empty()) {
      return {
        .error_code = make_error_code(llm_error_code::invalid_request),
        .message = "LLM stop sequence must not be empty",
      };
    }
  }
  if (capabilities.declared && !request.stop_sequences.empty() && !capabilities.stop_sequences) {
    return {
      .error_code = make_error_code(llm_error_code::unsupported_capability),
      .message = "LLM provider does not support stop sequences",
      .capability = "stop_sequences",
    };
  }
  if (capabilities.declared && request.seed && !capabilities.deterministic_seed) {
    return {
      .error_code = make_error_code(llm_error_code::unsupported_capability),
      .message = "LLM provider does not support deterministic seed",
      .capability = "deterministic_seed",
    };
  }
  if (request.response_format && request.json_schema_output) {
    return {
      .error_code = make_error_code(llm_error_code::invalid_request),
      .message = "response_format and json_schema_output are mutually exclusive",
    };
  }
  if (request.json_schema_output) {
    if (request.json_schema_output->name.empty() ||
        !request.json_schema_output->schema.is_object() ||
        request.json_schema_output->schema.empty()) {
      return {
        .error_code = make_error_code(llm_error_code::invalid_request),
        .message = "JSON schema output requires a name and non-empty object schema",
      };
    }
    if (capabilities.declared && !capabilities.json_schema_output) {
      return {
        .error_code = make_error_code(llm_error_code::unsupported_capability),
        .message = "LLM provider does not support JSON schema output",
        .capability = "json_schema_output",
      };
    }
  }
  if (request.cache_mode != ::wuwe::llm_cache_mode::provider_default && capabilities.declared &&
      !capabilities.explicit_cache_control) {
    return {
      .error_code = make_error_code(llm_error_code::unsupported_capability),
      .message = "LLM provider does not support explicit cache control",
      .capability = "explicit_cache_control",
    };
  }
  return {};
}

[[nodiscard]] inline std::optional<::wuwe::llm_response> llm_request_rejection(
  const ::wuwe::llm_request& request, const ::wuwe::llm_provider_capabilities& capabilities) {
  const auto validation = validate_llm_request(request, capabilities);
  if (validation) {
    return std::nullopt;
  }
  ::wuwe::llm_response response {
    .content = validation.message,
    .error_code = validation.error_code,
  };
  if (!validation.capability.empty()) {
    response.metadata["unsupported_capability"] = validation.capability;
  }
  return response;
}

inline void emit_llm_request_rejection(
  const ::wuwe::llm_stream_callbacks& callbacks, const ::wuwe::llm_response& response) {
  if (callbacks.on_event) {
    callbacks.on_event({
      .type = ::wuwe::llm_stream_event_type::error,
      .response = response,
      .error_code = response.error_code,
      .message = response.content,
    });
  }
}

} // namespace wuwe::agent::llm

#endif // WUWE_AGENT_LLM_LLM_CAPABILITIES_HPP
