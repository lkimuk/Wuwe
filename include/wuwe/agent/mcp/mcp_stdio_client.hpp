#ifndef WUWE_AGENT_MCP_STDIO_CLIENT_HPP
#define WUWE_AGENT_MCP_STDIO_CLIENT_HPP

#include <istream>
#include <ostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <wuwe/agent/mcp/mcp_protocol.hpp>
#include <wuwe/agent/mcp/mcp_stdio_transport.hpp>

namespace wuwe::agent::mcp {

class mcp_stdio_client {
public:
  mcp_stdio_client(std::istream& input, std::ostream& output)
      : input_(input), output_(output) {
  }

  json request(std::string method, json params = json::object()) {
    const auto id = next_id_++;
    const json request_message {
      { "jsonrpc", "2.0" },
      { "id", id },
      { "method", std::move(method) },
      { "params", std::move(params) },
    };
    mcp_stdio_transport::write_framed_message(output_, request_message.dump());
    return read_response(id);
  }

  void notify(std::string method, json params = json::object()) {
    mcp_stdio_transport::write_framed_message(output_, make_notification(
      std::move(method), std::move(params)));
  }

  json initialize(mcp_client_info info = {}, json capabilities = json::object()) {
    json client_info = json::object();
    if (!info.name.empty()) {
      client_info["name"] = std::move(info.name);
    }
    if (!info.version.empty()) {
      client_info["version"] = std::move(info.version);
    }

    return request("initialize", {
      { "protocolVersion", std::string(default_protocol_version) },
      { "clientInfo", std::move(client_info) },
      { "capabilities", std::move(capabilities) },
    });
  }

  json list_tools(json params = json::object()) {
    return request("tools/list", std::move(params));
  }

  json ping() {
    return request("ping");
  }

  json call_tool(std::string name, json arguments = json::object()) {
    return request("tools/call", {
      { "name", std::move(name) },
      { "arguments", std::move(arguments) },
    });
  }

  json list_resources(json params = json::object()) {
    return request("resources/list", std::move(params));
  }

  json read_resource(std::string uri) {
    return request("resources/read", { { "uri", std::move(uri) } });
  }

  json subscribe_resource(std::string uri) {
    return request("resources/subscribe", { { "uri", std::move(uri) } });
  }

  json unsubscribe_resource(std::string uri) {
    return request("resources/unsubscribe", { { "uri", std::move(uri) } });
  }

  json list_resource_templates(json params = json::object()) {
    return request("resources/templates/list", std::move(params));
  }

  json list_roots(json params = json::object()) {
    return request("roots/list", std::move(params));
  }

  json list_prompts(json params = json::object()) {
    return request("prompts/list", std::move(params));
  }

  json get_prompt(std::string name, json arguments = json::object()) {
    return request("prompts/get", {
      { "name", std::move(name) },
      { "arguments", std::move(arguments) },
    });
  }

  const std::vector<json>& notifications() const noexcept {
    return notifications_;
  }

  void clear_notifications() {
    notifications_.clear();
  }

private:
  json read_response(int id) {
    while (true) {
      const auto message = mcp_stdio_transport::read_framed_message(input_);
      if (!message) {
        throw std::runtime_error("MCP stdio client reached end of input before response");
      }

      json parsed = json::parse(*message);
      if (parsed.is_object() && !parsed.contains("id") && parsed.contains("method")) {
        notifications_.push_back(std::move(parsed));
        continue;
      }

      if (!parsed.is_object() || !parsed.contains("id")) {
        throw std::runtime_error("MCP stdio client received invalid response");
      }
      if (parsed["id"] != id) {
        notifications_.push_back(std::move(parsed));
        continue;
      }
      return parsed;
    }
  }

  std::istream& input_;
  std::ostream& output_;
  int next_id_ { 1 };
  std::vector<json> notifications_;
};

} // namespace wuwe::agent::mcp

#endif // WUWE_AGENT_MCP_STDIO_CLIENT_HPP
