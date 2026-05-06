#ifndef WUWE_AGENT_MCP_PROTOCOL_HPP
#define WUWE_AGENT_MCP_PROTOCOL_HPP

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace wuwe::agent::mcp {

using json = nlohmann::json;

inline constexpr const char* default_protocol_version = "2024-11-05";

enum class mcp_error_code : int {
  parse_error = -32700,
  invalid_request = -32600,
  method_not_found = -32601,
  invalid_params = -32602,
  internal_error = -32603,
  request_denied = -32001,
};

struct mcp_error {
  mcp_error_code code { mcp_error_code::internal_error };
  std::string message;
  std::optional<json> data;
};

struct mcp_server_info {
  std::string name { "wuwe" };
  std::string version { "0.1.0" };
};

struct mcp_client_info {
  std::string name;
  std::string version;
};

inline bool is_valid_jsonrpc_id(const json& id) {
  return id.is_null() || id.is_string() || id.is_number_integer() || id.is_number_unsigned();
}

inline json make_error_object(const mcp_error& error) {
  json output {
    { "code", static_cast<int>(error.code) },
    { "message", error.message },
  };
  if (error.data) {
    output["data"] = *error.data;
  }
  return output;
}

inline std::string make_success_response(const json& id, json result) {
  return json {
    { "jsonrpc", "2.0" },
    { "id", id },
    { "result", std::move(result) },
  }.dump();
}

inline std::string make_error_response(const json& id, const mcp_error& error) {
  return json {
    { "jsonrpc", "2.0" },
    { "id", id },
    { "error", make_error_object(error) },
  }.dump();
}

inline std::string make_request(json id, std::string method, json params = json::object()) {
  return json {
    { "jsonrpc", "2.0" },
    { "id", std::move(id) },
    { "method", std::move(method) },
    { "params", std::move(params) },
  }.dump();
}

inline std::string make_notification(std::string method, json params = json::object()) {
  return json {
    { "jsonrpc", "2.0" },
    { "method", std::move(method) },
    { "params", std::move(params) },
  }.dump();
}

} // namespace wuwe::agent::mcp

#endif // WUWE_AGENT_MCP_PROTOCOL_HPP
