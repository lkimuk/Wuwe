#include <string>
#include <string_view>
#include <vector>

#include <wuwe/agent/mcp/mcp_server.hpp>
#include <wuwe/agent/mcp/mcp_stdio_transport.hpp>
#include <wuwe/agent/tools/tool.hpp>

struct echo_text {
  static constexpr std::string_view description =
    "Echo a text string through the MCP tools/call method.";

  std::string text;

  std::string invoke() const {
    return text;
  }
};

int main() {
  wuwe::tool_provider<echo_text> tools;
  wuwe::agent::mcp::mcp_server server({ .name = "wuwe-mcp-example", .version = "0.1.0" });
  server.add_tool_provider(tools);
  server.add_mcp_tool(
    {
      .name = "preview_image",
      .description = "Return a tiny image payload.",
      .parameters_json_schema = R"({"type":"object","properties":{},"additionalProperties":false})",
    },
    [](const wuwe::agent::mcp::json&) {
      return wuwe::agent::mcp::mcp_tool_call_result {
        .content = {
          {
            .type = "image",
            .data = "iVBORw0KGgo=",
            .mime_type = "image/png",
          },
        },
      };
    });
  server.add_resource(
    {
      .uri = "wuwe://example/readme",
      .name = "Example README",
      .description = "A small text resource exposed by the MCP stdio example.",
      .mime_type = "text/plain",
    },
    [] {
      return std::vector<wuwe::agent::mcp::mcp_resource_content> {
        {
          .uri = "wuwe://example/readme",
          .mime_type = "text/plain",
          .text = "This resource is served by the Wuwe MCP stdio example.",
        },
      };
    });
  server.add_resource_template({
    .uri_template = "wuwe://example/{name}",
    .name = "Example Resource",
    .description = "Template metadata for example resources.",
    .mime_type = "text/plain",
  });
  server.add_prompt(
    {
      .name = "echo_prompt",
      .description = "Create a user message that echoes a topic.",
      .arguments = {
        {
          .name = "topic",
          .description = "Topic to echo.",
          .required = false,
        },
      },
    },
    [](const wuwe::agent::mcp::json& arguments) {
      return wuwe::agent::mcp::mcp_prompt_result {
        .description = "Echo prompt.",
        .messages = {
          {
            .role = "user",
            .text = "Echo this topic: " + arguments.value("topic", std::string("Wuwe MCP")),
          },
        },
      };
    });

  wuwe::agent::mcp::mcp_stdio_transport transport;
  return transport.run_stdio(server);
}
