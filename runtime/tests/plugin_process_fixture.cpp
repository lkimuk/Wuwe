#include <cstdlib>
#include <string>

#include <wuwe/agent/mcp/mcp_server.hpp>
#include <wuwe/agent/mcp/mcp_stdio_transport.hpp>

namespace {

std::string environment_value(const std::string& name) {
#ifdef _WIN32
  char* value = nullptr;
  std::size_t size = 0;
  if (_dupenv_s(&value, &size, name.c_str()) != 0 || !value)
    return {};
  std::string result(value);
  std::free(value);
  return result;
#else
  const auto* value = std::getenv(name.c_str());
  return value ? value : "";
#endif
}

} // namespace

int main() {
  wuwe::agent::mcp::mcp_server server({
    .name = "wuwe-runtime-plugin-fixture",
    .version = "1.0.0",
  });
  server.add_mcp_tool(
    {
      .name = "echo_text",
      .description = "Echo a string for Runtime plugin process tests.",
      .parameters_json_schema =
        R"({"type":"object","properties":{"text":{"type":"string"}},"required":["text"],"additionalProperties":false})",
    },
    [](const nlohmann::json& arguments) {
      return wuwe::agent::mcp::mcp_tool_call_result::text(arguments.at("text").get<std::string>());
    });
  server.add_mcp_tool(
    {
      .name = "environment_value",
      .description = "Read an environment value for Runtime plugin process tests.",
      .parameters_json_schema =
        R"({"type":"object","properties":{"name":{"type":"string"}},"required":["name"],"additionalProperties":false})",
    },
    [](const nlohmann::json& arguments) {
      const auto name = arguments.at("name").get<std::string>();
      return wuwe::agent::mcp::mcp_tool_call_result::text(environment_value(name));
    });

  wuwe::agent::mcp::mcp_stdio_transport transport;
  return transport.run_stdio(server);
}
