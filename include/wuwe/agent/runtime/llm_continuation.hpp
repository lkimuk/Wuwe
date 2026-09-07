#ifndef WUWE_AGENT_RUNTIME_LLM_CONTINUATION_HPP
#define WUWE_AGENT_RUNTIME_LLM_CONTINUATION_HPP

#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <wuwe/agent/llm/llm_capabilities.hpp>
#include <wuwe/agent/llm/llm_usage.hpp>
#include <wuwe/agent/llm/tool_output_projection.hpp>

namespace wuwe::agent::runtime {

struct llm_tool_continuation {
  llm_request request;
  std::vector<llm_tool_call> pending_calls;
  std::vector<std::string> approved_call_ids;
  int used_tool_rounds { 0 };
  bool assistant_persisted { false };
  llm_usage accumulated_usage;
  std::optional<agent::llm::llm_pricing> pricing;
  agent::llm::tool_output_projection_policy tool_output_projection;
};

namespace llm_codec {

inline llm_context_source context_source_from_json(const nlohmann::json& value) {
  if (value.is_number_integer()) {
    const auto source = value.get<int>();
    if (source >= static_cast<int>(llm_context_source::automatic) &&
        source <= static_cast<int>(llm_context_source::skill)) {
      return static_cast<llm_context_source>(source);
    }
  }
  if (value.is_string()) {
    const auto source = value.get<std::string>();
    if (source == "automatic")
      return llm_context_source::automatic;
    if (source == "system")
      return llm_context_source::system;
    if (source == "conversation")
      return llm_context_source::conversation;
    if (source == "memory")
      return llm_context_source::memory;
    if (source == "knowledge")
      return llm_context_source::knowledge;
    if (source == "skill")
      return llm_context_source::skill;
    if (source == "tool_result")
      return llm_context_source::tool_result;
    if (source == "other")
      return llm_context_source::other;
  }
  throw std::invalid_argument("invalid persisted LLM context source");
}

inline llm_context_overflow_policy context_overflow_from_json(const nlohmann::json& value) {
  if (value.is_number_integer()) {
    const auto policy = value.get<int>();
    if (policy >= static_cast<int>(llm_context_overflow_policy::reject) &&
        policy <= static_cast<int>(llm_context_overflow_policy::trim_low_priority)) {
      return static_cast<llm_context_overflow_policy>(policy);
    }
  }
  if (value.is_string()) {
    const auto policy = value.get<std::string>();
    if (policy == "reject")
      return llm_context_overflow_policy::reject;
    if (policy == "trim_low_priority") {
      return llm_context_overflow_policy::trim_low_priority;
    }
  }
  throw std::invalid_argument("invalid persisted context overflow policy");
}

inline llm_cache_mode cache_mode_from_json(const nlohmann::json& value) {
  if (!value.is_string()) {
    throw std::invalid_argument("invalid persisted LLM cache mode");
  }
  const auto mode = value.get<std::string>();
  if (mode == "provider_default")
    return llm_cache_mode::provider_default;
  if (mode == "disabled")
    return llm_cache_mode::disabled;
  if (mode == "enabled")
    return llm_cache_mode::enabled;
  throw std::invalid_argument("invalid persisted LLM cache mode");
}

inline nlohmann::json tool_call_to_json(const llm_tool_call& value) {
  return {
    { "id", value.id },
    { "name", value.name },
    { "arguments_json", value.arguments_json },
  };
}

inline llm_tool_call tool_call_from_json(const nlohmann::json& value) {
  return {
    .id = value.value("id", std::string {}),
    .name = value.value("name", std::string {}),
    .arguments_json = value.value("arguments_json", std::string("{}")),
  };
}

inline nlohmann::json message_to_json(const chat_message& value) {
  auto calls = nlohmann::json::array();
  for (const auto& call : value.tool_calls) {
    calls.push_back(tool_call_to_json(call));
  }
  nlohmann::json output {
    { "role", value.role },
    { "content", value.content },
    { "reasoning_content", value.reasoning_content },
    { "tool_calls", std::move(calls) },
    { "context_source", std::string(to_string(value.context_source)) },
  };
  output["name"] = value.name ? nlohmann::json(*value.name) : nlohmann::json(nullptr);
  output["tool_call_id"] =
    value.tool_call_id ? nlohmann::json(*value.tool_call_id) : nlohmann::json(nullptr);
  return output;
}

inline chat_message message_from_json(const nlohmann::json& value) {
  chat_message message;
  message.role = value.value("role", std::string {});
  message.content = value.value("content", std::string {});
  message.reasoning_content = value.value("reasoning_content", std::string {});
  if (value.contains("name") && !value.at("name").is_null()) {
    message.name = value.at("name").get<std::string>();
  }
  if (value.contains("tool_call_id") && !value.at("tool_call_id").is_null()) {
    message.tool_call_id = value.at("tool_call_id").get<std::string>();
  }
  if (value.contains("tool_calls") && value.at("tool_calls").is_array()) {
    for (const auto& call : value.at("tool_calls")) {
      message.tool_calls.push_back(tool_call_from_json(call));
    }
  }
  message.context_source = value.contains("context_source")
                             ? context_source_from_json(value.at("context_source"))
                             : llm_context_source::automatic;
  return message;
}

inline nlohmann::json request_to_json(const llm_request& value) {
  auto messages = nlohmann::json::array();
  for (const auto& message : value.messages) {
    messages.push_back(message_to_json(message));
  }
  auto tools = nlohmann::json::array();
  for (const auto& tool : value.tools) {
    tools.push_back({
      { "name", tool.name },
      { "description", tool.description },
      { "parameters_json_schema", tool.parameters_json_schema },
    });
  }
  nlohmann::json output {
    { "model", value.model },
    { "provider", value.provider },
    { "messages", std::move(messages) },
    { "temperature", value.temperature },
    { "tools", std::move(tools) },
    { "language",
      {
        { "response_language", value.language.response_language },
        { "reasoning_language", value.language.reasoning_language },
        { "locale", value.language.locale },
      } },
  };
  output["response_format"] =
    value.response_format ? nlohmann::json(*value.response_format) : nlohmann::json(nullptr);
  output["max_output_tokens"] =
    value.max_output_tokens ? nlohmann::json(*value.max_output_tokens) : nlohmann::json(nullptr);
  output["thinking_mode"] = std::string(to_string(value.thinking_mode));
  output["stop_sequences"] = value.stop_sequences;
  output["seed"] = value.seed ? nlohmann::json(*value.seed) : nlohmann::json(nullptr);
  output["json_schema_output"] = value.json_schema_output
                                   ? nlohmann::json({
                                       { "name", value.json_schema_output->name },
                                       { "schema", value.json_schema_output->schema },
                                       { "strict", value.json_schema_output->strict },
                                     })
                                   : nlohmann::json(nullptr);
  output["cache_mode"] = std::string(to_string(value.cache_mode));
  output["execution_context"] = value.execution_context
                                  ? core::execution_context_to_json(*value.execution_context)
                                  : nlohmann::json(nullptr);
  if (value.context_budget) {
    output["context_budget"] = {
      { "context_window_tokens", value.context_budget->context_window_tokens },
      { "reserved_output_tokens", value.context_budget->reserved_output_tokens },
      { "minimum_recent_conversation_messages",
        value.context_budget->minimum_recent_conversation_messages },
      { "minimum_recent_tool_exchanges", value.context_budget->minimum_recent_tool_exchanges },
      { "overflow", std::string(to_string(value.context_budget->overflow)) },
      { "allow_system_truncation", value.context_budget->allow_system_truncation },
      { "limits",
        {
          { "system", value.context_budget->limits.system },
          { "conversation", value.context_budget->limits.conversation },
          { "memory", value.context_budget->limits.memory },
          { "knowledge", value.context_budget->limits.knowledge },
          { "skills", value.context_budget->limits.skills },
          { "tool_schemas", value.context_budget->limits.tool_schemas },
          { "tool_results", value.context_budget->limits.tool_results },
          { "other", value.context_budget->limits.other },
        } },
    };
  }
  else {
    output["context_budget"] = nullptr;
  }
  if (value.tool_choice) {
    output["tool_choice"] = {
      { "mode", static_cast<int>(value.tool_choice->mode) },
      { "name", value.tool_choice->name },
    };
  }
  else {
    output["tool_choice"] = nullptr;
  }
  return output;
}

inline llm_request request_from_json(const nlohmann::json& value) {
  llm_request request;
  request.model = value.value("model", std::string {});
  request.provider = value.value("provider", std::string {});
  request.temperature = value.value("temperature", 0.2);
  if (value.contains("messages") && value.at("messages").is_array()) {
    for (const auto& message : value.at("messages")) {
      request.messages.push_back(message_from_json(message));
    }
  }
  if (value.contains("tools") && value.at("tools").is_array()) {
    for (const auto& tool : value.at("tools")) {
      request.tools.push_back({
        .name = tool.value("name", std::string {}),
        .description = tool.value("description", std::string {}),
        .parameters_json_schema = tool.value("parameters_json_schema", std::string("{}")),
      });
    }
  }
  if (value.contains("response_format") && !value.at("response_format").is_null()) {
    request.response_format = value.at("response_format").get<std::string>();
  }
  if (value.contains("max_output_tokens") && !value.at("max_output_tokens").is_null()) {
    request.max_output_tokens = value.at("max_output_tokens").get<int>();
  }
  const auto thinking_mode = value.value("thinking_mode", std::string("provider_default"));
  request.thinking_mode = thinking_mode == "enabled"
                            ? llm_thinking_mode::enabled
                            : thinking_mode == "disabled"
                                ? llm_thinking_mode::disabled
                                : llm_thinking_mode::provider_default;
  request.stop_sequences = value.value("stop_sequences", std::vector<std::string> {});
  if (value.contains("seed") && !value.at("seed").is_null()) {
    request.seed = value.at("seed").get<std::int64_t>();
  }
  if (value.contains("json_schema_output") && !value.at("json_schema_output").is_null()) {
    const auto& output = value.at("json_schema_output");
    request.json_schema_output = llm_json_schema_output {
      .name = output.value("name", std::string {}),
      .schema = output.value("schema", nlohmann::json::object()),
      .strict = output.value("strict", true),
    };
  }
  request.cache_mode = value.contains("cache_mode") ? cache_mode_from_json(value.at("cache_mode"))
                                                    : llm_cache_mode::provider_default;
  if (value.contains("execution_context") && !value.at("execution_context").is_null()) {
    request.execution_context = core::execution_context_from_json(value.at("execution_context"));
  }
  if (value.contains("context_budget") && !value.at("context_budget").is_null()) {
    const auto& encoded = value.at("context_budget");
    const auto overflow = encoded.contains("overflow")
                            ? context_overflow_from_json(encoded.at("overflow"))
                            : llm_context_overflow_policy::trim_low_priority;
    const auto limits = encoded.value("limits", nlohmann::json::object());
    request.context_budget = llm_context_budget {
      .context_window_tokens = encoded.value("context_window_tokens", std::size_t {}),
      .reserved_output_tokens = encoded.value("reserved_output_tokens", std::size_t { 1024 }),
      .minimum_recent_conversation_messages = encoded.value(
        "minimum_recent_conversation_messages", std::size_t { 2 }),
      .limits = {
        .system = limits.value("system", std::size_t {}),
        .conversation = limits.value("conversation", std::size_t {}),
        .memory = limits.value("memory", std::size_t {}),
        .knowledge = limits.value("knowledge", std::size_t {}),
        .skills = limits.value("skills", std::size_t {}),
        .tool_schemas = limits.value("tool_schemas", std::size_t {}),
        .tool_results = limits.value("tool_results", std::size_t {}),
        .other = limits.value("other", std::size_t {}),
      },
      .overflow = overflow,
      .allow_system_truncation = encoded.value("allow_system_truncation", false),
      .minimum_recent_tool_exchanges =
        encoded.value("minimum_recent_tool_exchanges", std::size_t { 1 }),
    };
  }
  if (value.contains("tool_choice") && !value.at("tool_choice").is_null()) {
    const auto mode = value.at("tool_choice").value("mode", 0);
    if (mode < static_cast<int>(llm_tool_choice_mode::auto_) ||
        mode > static_cast<int>(llm_tool_choice_mode::named)) {
      throw std::invalid_argument("invalid persisted LLM tool choice mode");
    }
    request.tool_choice = llm_tool_choice {
      .mode = static_cast<llm_tool_choice_mode>(mode),
      .name = value.at("tool_choice").value("name", std::string {}),
    };
  }
  if (value.contains("language") && value.at("language").is_object()) {
    const auto& language = value.at("language");
    request.language.response_language = language.value("response_language", std::string {});
    request.language.reasoning_language = language.value("reasoning_language", std::string {});
    request.language.locale = language.value("locale", std::string {});
  }
  const auto validation = agent::llm::validate_llm_request(request,
    {
      .tools = true,
      .tool_choice = true,
      .json_response_format = true,
      .stop_sequences = true,
      .deterministic_seed = true,
      .json_schema_output = true,
      .explicit_cache_control = true,
    });
  if (!validation) {
    throw std::invalid_argument("invalid persisted LLM request: " + validation.message);
  }
  return request;
}

inline nlohmann::json usage_to_json(const llm_usage& value) {
  return {
    { "prompt_tokens", value.prompt_tokens },
    { "completion_tokens", value.completion_tokens },
    { "total_tokens", value.total_tokens },
    { "cached_prompt_tokens", value.cached_prompt_tokens },
    { "reasoning_tokens", value.reasoning_tokens },
  };
}

inline llm_usage usage_from_json(const nlohmann::json& value) {
  llm_usage usage {
    .prompt_tokens = value.value("prompt_tokens", 0),
    .completion_tokens = value.value("completion_tokens", 0),
    .total_tokens = value.value("total_tokens", 0),
    .cached_prompt_tokens = value.value("cached_prompt_tokens", 0),
    .reasoning_tokens = value.value("reasoning_tokens", 0),
  };
  if (usage.prompt_tokens < 0 || usage.completion_tokens < 0 || usage.total_tokens < 0 ||
      usage.cached_prompt_tokens < 0 || usage.reasoning_tokens < 0 ||
      usage.cached_prompt_tokens > usage.prompt_tokens ||
      usage.reasoning_tokens > usage.completion_tokens) {
    throw std::invalid_argument("invalid persisted LLM usage");
  }
  return usage;
}

inline nlohmann::json pricing_to_json(const agent::llm::llm_pricing& value) {
  nlohmann::json output {
    { "input_per_million_tokens_usd", value.input_per_million_tokens_usd },
    { "output_per_million_tokens_usd", value.output_per_million_tokens_usd },
  };
  output["cached_input_per_million_tokens_usd"] =
    value.cached_input_per_million_tokens_usd
      ? nlohmann::json(*value.cached_input_per_million_tokens_usd)
      : nlohmann::json(nullptr);
  output["reasoning_per_million_tokens_usd"] =
    value.reasoning_per_million_tokens_usd ? nlohmann::json(*value.reasoning_per_million_tokens_usd)
                                           : nlohmann::json(nullptr);
  return output;
}

inline agent::llm::llm_pricing pricing_from_json(const nlohmann::json& value) {
  agent::llm::llm_pricing pricing {
    .input_per_million_tokens_usd = value.value("input_per_million_tokens_usd", 0.0),
    .output_per_million_tokens_usd = value.value("output_per_million_tokens_usd", 0.0),
  };
  if (value.contains("cached_input_per_million_tokens_usd") &&
      !value.at("cached_input_per_million_tokens_usd").is_null()) {
    pricing.cached_input_per_million_tokens_usd =
      value.at("cached_input_per_million_tokens_usd").get<double>();
  }
  if (value.contains("reasoning_per_million_tokens_usd") &&
      !value.at("reasoning_per_million_tokens_usd").is_null()) {
    pricing.reasoning_per_million_tokens_usd =
      value.at("reasoning_per_million_tokens_usd").get<double>();
  }
  if (!agent::llm::valid_llm_pricing(pricing)) {
    throw std::invalid_argument("invalid persisted LLM pricing");
  }
  return pricing;
}

inline nlohmann::json tool_output_projection_to_json(
  const agent::llm::tool_output_projection_policy& value) {
  agent::llm::validate_tool_output_projection_policy(value);
  return {
    { "max_bytes", value.max_bytes },
    { "max_tokens", value.max_tokens },
  };
}

inline agent::llm::tool_output_projection_policy tool_output_projection_from_json(
  const nlohmann::json& value) {
  agent::llm::tool_output_projection_policy policy {
    .max_bytes = value.value("max_bytes", agent::llm::default_tool_output_projection_max_bytes),
    .max_tokens = value.value("max_tokens", agent::llm::default_tool_output_projection_max_tokens),
  };
  agent::llm::validate_tool_output_projection_policy(policy);
  return policy;
}

} // namespace llm_codec

inline nlohmann::json llm_continuation_to_json(const llm_tool_continuation& value) {
  auto pending = nlohmann::json::array();
  for (const auto& call : value.pending_calls) {
    pending.push_back(llm_codec::tool_call_to_json(call));
  }
  nlohmann::json output {
    { "schema_version", 2 },
    { "kind", "llm_tool_loop" },
    { "request", llm_codec::request_to_json(value.request) },
    { "pending_calls", std::move(pending) },
    { "approved_call_ids", value.approved_call_ids },
    { "used_tool_rounds", value.used_tool_rounds },
    { "assistant_persisted", value.assistant_persisted },
    { "accumulated_usage", llm_codec::usage_to_json(value.accumulated_usage) },
  };
  output["pricing"] =
    value.pricing ? llm_codec::pricing_to_json(*value.pricing) : nlohmann::json(nullptr);
  output["tool_output_projection"] =
    llm_codec::tool_output_projection_to_json(value.tool_output_projection);
  return output;
}

inline llm_tool_continuation llm_continuation_from_json(const nlohmann::json& value) {
  const auto schema_version = value.value("schema_version", 0);
  if ((schema_version != 1 && schema_version != 2) ||
      value.value("kind", std::string {}) != "llm_tool_loop") {
    throw std::invalid_argument("unsupported LLM continuation payload");
  }
  llm_tool_continuation continuation;
  continuation.request = llm_codec::request_from_json(value.at("request"));
  if (value.contains("pending_calls") && value.at("pending_calls").is_array()) {
    for (const auto& call : value.at("pending_calls")) {
      continuation.pending_calls.push_back(llm_codec::tool_call_from_json(call));
    }
  }
  continuation.approved_call_ids = value.value("approved_call_ids", std::vector<std::string> {});
  continuation.used_tool_rounds = value.value("used_tool_rounds", 0);
  continuation.assistant_persisted = value.value("assistant_persisted", false);
  continuation.accumulated_usage =
    llm_codec::usage_from_json(value.value("accumulated_usage", nlohmann::json::object()));
  if (value.contains("pricing") && !value.at("pricing").is_null()) {
    continuation.pricing = llm_codec::pricing_from_json(value.at("pricing"));
  }
  if (schema_version >= 2 && value.contains("tool_output_projection") &&
      !value.at("tool_output_projection").is_null()) {
    continuation.tool_output_projection =
      llm_codec::tool_output_projection_from_json(value.at("tool_output_projection"));
  }
  if (continuation.used_tool_rounds < 0) {
    throw std::invalid_argument("persisted LLM continuation has negative tool rounds");
  }
  std::set<std::string> pending_ids;
  for (const auto& call : continuation.pending_calls) {
    if (call.id.empty() || call.name.empty() || !pending_ids.insert(call.id).second) {
      throw std::invalid_argument("persisted LLM continuation has invalid pending tool calls");
    }
  }
  std::set<std::string> approved_ids;
  for (const auto& call_id : continuation.approved_call_ids) {
    if (!pending_ids.contains(call_id) || !approved_ids.insert(call_id).second) {
      throw std::invalid_argument("persisted LLM continuation has invalid approved tool calls");
    }
  }
  return continuation;
}

} // namespace wuwe::agent::runtime

#endif // WUWE_AGENT_RUNTIME_LLM_CONTINUATION_HPP
