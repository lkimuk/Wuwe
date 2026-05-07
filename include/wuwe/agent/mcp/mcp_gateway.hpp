#ifndef WUWE_AGENT_MCP_GATEWAY_HPP
#define WUWE_AGENT_MCP_GATEWAY_HPP

#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include <wuwe/agent/mcp/mcp_host_runtime.hpp>
#include <wuwe/agent/mcp/mcp_server.hpp>

namespace wuwe::agent::mcp {

struct mcp_gateway_options {
  std::string separator { "__" };
};

class mcp_gateway {
public:
  explicit mcp_gateway(mcp_gateway_options options = {})
      : options_(std::move(options)) {
  }

  void populate_server(
    mcp_server& server,
    mcp_host_runtime& runtime) const {
    for (const auto& snapshot : runtime.snapshots()) {
      if (!snapshot.running) {
        continue;
      }
      add_server_tools(server, runtime, snapshot.id);
      add_server_resources(server, runtime, snapshot.id);
      add_server_prompts(server, runtime, snapshot.id);
    }
  }

  static mcp_server make_server(
    mcp_server_info info = { .name = "wuwe-mcp-gateway", .version = "0.1.0" }) {
    return mcp_server(std::move(info));
  }

  std::string gateway_name(const std::string& server_id, const std::string& name) const {
    return server_id + options_.separator + name;
  }

private:
  void add_server_tools(
    mcp_server& server,
    mcp_host_runtime& runtime,
    const std::string& server_id) const {
    const auto tools = runtime.list_tools(server_id);
    for (const auto& item : tools["result"].value("tools", nlohmann::json::array())) {
      if (!item.is_object() || !item.value("name", std::string()).size()) {
        continue;
      }
      const auto original_name = item.value("name", std::string());
      llm_tool tool {
        .name = gateway_name(server_id, original_name),
        .description = item.value("description", std::string()),
        .parameters_json_schema = item.value("inputSchema", nlohmann::json::object()).dump(),
      };
      server.add_mcp_tool(std::move(tool),
        [&runtime, server_id, original_name](const nlohmann::json& arguments) {
          const auto response = runtime.call_tool(server_id, original_name, arguments);
          return tool_call_result_from_response(response);
        });
    }
  }

  void add_server_resources(
    mcp_server& server,
    mcp_host_runtime& runtime,
    const std::string& server_id) const {
    const auto resources = runtime.list_resources(server_id);
    for (const auto& item : resources["result"].value("resources", nlohmann::json::array())) {
      if (!item.is_object() || !item.value("uri", std::string()).size()) {
        continue;
      }
      const auto original_uri = item.value("uri", std::string());
      mcp_resource resource {
        .uri = gateway_resource_uri(server_id, original_uri),
        .name = gateway_name(server_id, item.value("name", original_uri)),
        .description = item.value("description", std::string()),
        .mime_type = item.value("mimeType", std::string("text/plain")),
      };
      server.add_resource(std::move(resource), [&runtime, server_id, original_uri] {
        const auto response = runtime.read_resource(server_id, original_uri);
        return resource_contents_from_response(response);
      });
    }
  }

  void add_server_prompts(
    mcp_server& server,
    mcp_host_runtime& runtime,
    const std::string& server_id) const {
    const auto prompts = runtime.list_prompts(server_id);
    for (const auto& item : prompts["result"].value("prompts", nlohmann::json::array())) {
      if (!item.is_object() || !item.value("name", std::string()).size()) {
        continue;
      }
      const auto original_name = item.value("name", std::string());
      mcp_prompt prompt {
        .name = gateway_name(server_id, original_name),
        .description = item.value("description", std::string()),
      };
      for (const auto& argument : item.value("arguments", nlohmann::json::array())) {
        if (!argument.is_object() || !argument.value("name", std::string()).size()) {
          continue;
        }
        prompt.arguments.push_back({
          .name = argument.value("name", std::string()),
          .description = argument.value("description", std::string()),
          .required = argument.value("required", false),
        });
      }
      server.add_prompt(std::move(prompt),
        [&runtime, server_id, original_name](const nlohmann::json& arguments) {
          const auto response = runtime.get_prompt(server_id, original_name, arguments);
          return prompt_result_from_response(response);
        });
    }
  }

  std::string gateway_resource_uri(
    const std::string& server_id,
    const std::string& uri) const {
    return "mcp-gateway://" + server_id + "/" + uri;
  }

  static mcp_tool_call_result tool_call_result_from_response(const nlohmann::json& response) {
    const auto result = response.value("result", nlohmann::json::object());
    mcp_tool_call_result output {
      .is_error = result.value("isError", false),
    };
    for (const auto& item : result.value("content", nlohmann::json::array())) {
      output.content.push_back(content_from_json(item));
    }
    return output;
  }

  static std::vector<mcp_resource_content> resource_contents_from_response(
    const nlohmann::json& response) {
    std::vector<mcp_resource_content> output;
    const auto result = response.value("result", nlohmann::json::object());
    for (const auto& item : result.value("contents", nlohmann::json::array())) {
      output.push_back({
        .uri = item.value("uri", std::string()),
        .mime_type = item.value("mimeType", std::string("text/plain")),
        .text = item.value("text", std::string()),
        .blob = item.value("blob", std::string()),
      });
    }
    return output;
  }

  static mcp_prompt_result prompt_result_from_response(const nlohmann::json& response) {
    const auto result = response.value("result", nlohmann::json::object());
    mcp_prompt_result output {
      .description = result.value("description", std::string()),
    };
    for (const auto& item : result.value("messages", nlohmann::json::array())) {
      mcp_prompt_message message {
        .role = item.value("role", std::string("user")),
      };
      if (item.contains("content") && item["content"].is_object()) {
        message.content = content_from_json(item["content"]);
        message.text = message.content.text;
      }
      output.messages.push_back(std::move(message));
    }
    return output;
  }

  static mcp_content content_from_json(const nlohmann::json& value) {
    if (!value.is_object()) {
      return {};
    }
    return {
      .type = value.value("type", std::string("text")),
      .text = value.value("text", std::string()),
      .data = value.value("data", std::string()),
      .mime_type = value.value("mimeType", std::string()),
      .uri = value.value("uri", std::string()),
    };
  }

  mcp_gateway_options options_;
};

} // namespace wuwe::agent::mcp

#endif // WUWE_AGENT_MCP_GATEWAY_HPP
