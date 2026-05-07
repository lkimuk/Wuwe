#ifndef WUWE_AGENT_MCP_PROCESS_CLIENT_HPP
#define WUWE_AGENT_MCP_PROCESS_CLIENT_HPP

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <wuwe/agent/mcp/mcp_protocol.hpp>

namespace wuwe::agent::mcp {

struct mcp_process_command {
  std::string command;
  std::vector<std::string> args;
  std::string working_directory;
  std::map<std::string, std::string> environment;
};

class mcp_process_client {
public:
  mcp_process_client();
  explicit mcp_process_client(mcp_process_command command);
  ~mcp_process_client();

  mcp_process_client(const mcp_process_client&) = delete;
  mcp_process_client& operator=(const mcp_process_client&) = delete;

  mcp_process_client(mcp_process_client&&) noexcept;
  mcp_process_client& operator=(mcp_process_client&&) noexcept;

  void start(mcp_process_command command);
  void stop();
  bool running();

  json request(std::string method, json params = json::object());
  void notify(std::string method, json params = json::object());

  json initialize(mcp_client_info info = {}, json capabilities = json::object());
  json ping();
  json list_tools(json params = json::object());
  json call_tool(std::string name, json arguments = json::object());
  json list_resources(json params = json::object());
  json read_resource(std::string uri);
  json subscribe_resource(std::string uri);
  json unsubscribe_resource(std::string uri);
  json list_resource_templates(json params = json::object());
  json list_roots(json params = json::object());
  json list_prompts(json params = json::object());
  json get_prompt(std::string name, json arguments = json::object());

  const std::vector<json>& notifications() const noexcept;
  void clear_notifications();
  std::string stderr_output() const;
  void clear_stderr_output();

private:
  struct impl;

  json read_response(int id);

  std::unique_ptr<impl> impl_;
  int next_id_ { 1 };
  std::vector<json> notifications_;
};

} // namespace wuwe::agent::mcp

#endif // WUWE_AGENT_MCP_PROCESS_CLIENT_HPP
