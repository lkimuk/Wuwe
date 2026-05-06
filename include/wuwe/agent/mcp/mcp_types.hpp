#ifndef WUWE_AGENT_MCP_TYPES_HPP
#define WUWE_AGENT_MCP_TYPES_HPP

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <wuwe/agent/llm/llm_types.h>
#include <wuwe/agent/mcp/mcp_protocol.hpp>

namespace wuwe::agent::mcp {

struct mcp_content {
  std::string type { "text" };
  std::string text;
  std::string data;
  std::string mime_type;
  std::string uri;

  static mcp_content text_content(std::string value) {
    return { .type = "text", .text = std::move(value) };
  }

  static mcp_content image(std::string base64_data, std::string mime = "image/png") {
    return {
      .type = "image",
      .data = std::move(base64_data),
      .mime_type = std::move(mime),
    };
  }

  static mcp_content audio(std::string base64_data, std::string mime) {
    return {
      .type = "audio",
      .data = std::move(base64_data),
      .mime_type = std::move(mime),
    };
  }

  static mcp_content resource_link(std::string resource_uri, std::string label = {}) {
    return {
      .type = "resource_link",
      .text = std::move(label),
      .uri = std::move(resource_uri),
    };
  }
};

struct mcp_tool_call_result {
  std::vector<mcp_content> content;
  bool is_error { false };

  static mcp_tool_call_result text(std::string value) {
    return { .content = { mcp_content::text_content(std::move(value)) } };
  }

  static mcp_tool_call_result error(std::string value) {
    return {
      .content = { mcp_content::text_content(std::move(value)) },
      .is_error = true,
    };
  }
};

struct mcp_resource_content {
  std::string uri;
  std::string mime_type { "text/plain" };
  std::string text;
  std::string blob;

  static mcp_resource_content text_content(
    std::string resource_uri,
    std::string value,
    std::string mime = "text/plain") {
    return {
      .uri = std::move(resource_uri),
      .mime_type = std::move(mime),
      .text = std::move(value),
    };
  }

  static mcp_resource_content blob_content(
    std::string resource_uri,
    std::string base64_data,
    std::string mime) {
    return {
      .uri = std::move(resource_uri),
      .mime_type = std::move(mime),
      .blob = std::move(base64_data),
    };
  }
};

struct mcp_tool_entry {
  llm_tool tool;
  std::function<mcp_tool_call_result(const json& arguments)> invoke;
};

struct mcp_resource {
  std::string uri;
  std::string name;
  std::string description;
  std::string mime_type { "text/plain" };
};

struct mcp_resource_entry {
  mcp_resource resource;
  std::function<std::vector<mcp_resource_content>()> read;
};

struct mcp_resource_template {
  std::string uri_template;
  std::string name;
  std::string description;
  std::string mime_type { "text/plain" };
};

struct mcp_root {
  std::string uri;
  std::string name;
};

struct mcp_sampling_message {
  std::string role { "user" };
  mcp_content content;

  static mcp_sampling_message user_text(std::string value) {
    return { .role = "user", .content = mcp_content::text_content(std::move(value)) };
  }
};

struct mcp_sampling_request {
  std::vector<mcp_sampling_message> messages;
  std::optional<int> max_tokens;
  std::optional<double> temperature;
  json model_preferences { json::object() };
  json metadata { json::object() };

  json to_json() const {
    json output;
    output["messages"] = json::array();
    for (const auto& message : messages) {
      json content {
        { "type", message.content.type.empty() ? "text" : message.content.type },
      };
      if (!message.content.text.empty()) {
        content["text"] = message.content.text;
      }
      if (!message.content.data.empty()) {
        content["data"] = message.content.data;
      }
      if (!message.content.mime_type.empty()) {
        content["mimeType"] = message.content.mime_type;
      }
      if (!message.content.uri.empty()) {
        content["uri"] = message.content.uri;
      }
      output["messages"].push_back({
        { "role", message.role },
        { "content", std::move(content) },
      });
    }
    if (max_tokens) {
      output["maxTokens"] = *max_tokens;
    }
    if (temperature) {
      output["temperature"] = *temperature;
    }
    if (!model_preferences.empty()) {
      output["modelPreferences"] = model_preferences;
    }
    if (!metadata.empty()) {
      output["metadata"] = metadata;
    }
    return output;
  }
};

struct mcp_sampling_result {
  std::string role;
  mcp_content content;
  std::string model;
  std::string stop_reason;

  static std::optional<mcp_sampling_result> from_json(const json& value) {
    if (!value.is_object()) {
      return std::nullopt;
    }
    mcp_sampling_result result;
    result.role = value.value("role", std::string {});
    result.model = value.value("model", std::string {});
    result.stop_reason = value.value("stopReason", std::string {});
    const auto content = value.find("content");
    if (content != value.end() && content->is_object()) {
      result.content.type = content->value("type", std::string("text"));
      result.content.text = content->value("text", std::string {});
      result.content.data = content->value("data", std::string {});
      result.content.mime_type = content->value("mimeType", std::string {});
      result.content.uri = content->value("uri", std::string {});
    }
    return result;
  }
};

struct mcp_elicitation_request {
  std::string message;
  json requested_schema { json::object() };
  json metadata { json::object() };

  json to_json() const {
    json output {
      { "message", message },
    };
    if (!requested_schema.empty()) {
      output["requestedSchema"] = requested_schema;
    }
    if (!metadata.empty()) {
      output["metadata"] = metadata;
    }
    return output;
  }
};

struct mcp_elicitation_result {
  std::string action;
  json content { json::object() };

  static std::optional<mcp_elicitation_result> from_json(const json& value) {
    if (!value.is_object()) {
      return std::nullopt;
    }
    return mcp_elicitation_result {
      .action = value.value("action", std::string {}),
      .content = value.value("content", json::object()),
    };
  }
};

struct mcp_prompt_argument {
  std::string name;
  std::string description;
  bool required { false };
};

struct mcp_prompt_message {
  std::string role { "user" };
  std::string text;
  mcp_content content;

  static mcp_prompt_message user_text(std::string value) {
    return { .role = "user", .text = std::move(value) };
  }

  static mcp_prompt_message assistant_text(std::string value) {
    return { .role = "assistant", .text = std::move(value) };
  }

  static mcp_prompt_message user_content(mcp_content value) {
    return { .role = "user", .content = std::move(value) };
  }
};

struct mcp_prompt_result {
  std::string description;
  std::vector<mcp_prompt_message> messages;

  static mcp_prompt_result single_user_message(std::string value, std::string desc = {}) {
    return {
      .description = std::move(desc),
      .messages = { mcp_prompt_message::user_text(std::move(value)) },
    };
  }
};

struct mcp_prompt {
  std::string name;
  std::string description;
  std::vector<mcp_prompt_argument> arguments;
};

struct mcp_prompt_entry {
  mcp_prompt prompt;
  std::function<mcp_prompt_result(const json& arguments)> get;
};

struct mcp_pending_client_request {
  json id;
  std::string method;
  json params { json::object() };
  bool completed { false };
  json result;
  std::optional<mcp_error> error;
};

struct mcp_server_exchange {
  std::vector<std::string> requests;
  std::vector<std::string> notifications;
  std::optional<std::string> response;
};

} // namespace wuwe::agent::mcp

#endif // WUWE_AGENT_MCP_TYPES_HPP
