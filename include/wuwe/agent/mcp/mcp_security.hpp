#ifndef WUWE_AGENT_MCP_SECURITY_HPP
#define WUWE_AGENT_MCP_SECURITY_HPP

#include <string>
#include <vector>

#include <wuwe/agent/mcp/mcp_protocol.hpp>

namespace wuwe::agent::mcp {

struct mcp_auth_context {
  std::string subject;
  std::string issuer;
  std::string audience;
  std::vector<std::string> scopes;
  json claims { json::object() };
};

struct mcp_access_policy {
  bool allow_unlisted { true };
  std::string tenant_id;
  std::vector<std::string> scopes;
  std::vector<std::string> allowed_tools;
  std::vector<std::string> denied_tools;
  std::vector<std::string> allowed_resources;
  std::vector<std::string> denied_resources;
  std::vector<std::string> allowed_resource_templates;
  std::vector<std::string> denied_resource_templates;
  std::vector<std::string> allowed_prompts;
  std::vector<std::string> denied_prompts;
  std::vector<std::string> redacted_argument_keys {
    "api_key",
    "apikey",
    "authorization",
    "password",
    "secret",
    "token",
  };
};

struct mcp_audit_event {
  std::string action;
  std::string target;
  mcp_auth_context auth;
  std::string tenant_id;
  std::vector<std::string> scopes;
  bool allowed { false };
  std::string reason;
  json arguments { json::object() };
  bool redacted { false };
};

} // namespace wuwe::agent::mcp

#endif // WUWE_AGENT_MCP_SECURITY_HPP
