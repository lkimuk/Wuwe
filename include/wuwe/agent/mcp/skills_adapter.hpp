#ifndef WUWE_AGENT_MCP_SKILLS_ADAPTER_HPP
#define WUWE_AGENT_MCP_SKILLS_ADAPTER_HPP

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <wuwe/agent/mcp/detail/bound_tool_provider.hpp>
#include <wuwe/agent/mcp/mcp_host_runtime.hpp>

namespace wuwe::agent::mcp {

struct mcp_skill_tool_binding {
  std::string server_id;
  std::string tool_name;
  std::string exposed_name;
};

struct mcp_skill_tool_provider_options {
  std::string separator { "__" };
  bool require_running_server { true };
};

// Explicitly binds selected MCP tools into a normal Wuwe tool-provider
// boundary. It never imports every tool from a server implicitly.
class mcp_skill_tool_provider final {
public:
  mcp_skill_tool_provider(mcp_host_runtime& runtime, std::vector<mcp_skill_tool_binding> bindings,
    mcp_skill_tool_provider_options options = {})
      : provider_(
          {
            .running = [&runtime](const std::string& id) { return runtime.snapshot(id).running; },
            .list_tools =
              [&runtime](const std::string& id) { return runtime.list_tools(id); },
            .call_tool =
              [&runtime](const std::string& id, std::string name, nlohmann::json arguments) {
                return runtime.call_tool(id, std::move(name), std::move(arguments));
              },
          },
          bindings_from(std::move(bindings)),
          {
            .label = "MCP skill",
            .source_label = "server",
            .separator = std::move(options.separator),
            .require_running_source = options.require_running_server,
            .capability_name = "mcp.tool.call",
            .capability_summary = "Invoke an explicitly bound MCP server tool",
          }) {
  }

  [[nodiscard]] std::vector<tools::tool_descriptor> descriptors() const {
    return provider_.descriptors();
  }

  [[nodiscard]] std::vector<llm_tool> tools() const {
    return provider_.tools();
  }

  [[nodiscard]] tools::tool_provider_capabilities contract_capabilities(
    std::string_view name) const noexcept {
    return provider_.contract_capabilities(name);
  }

  llm_tool_result invoke(const std::string& name, const std::string& arguments_json) const {
    return provider_.invoke(name, arguments_json);
  }

  llm_tool_result invoke(const tools::tool_invocation& invocation) const {
    return provider_.invoke(invocation);
  }

private:
  static std::vector<detail::bound_tool_binding> bindings_from(
    std::vector<mcp_skill_tool_binding> bindings) {
    std::vector<detail::bound_tool_binding> output;
    output.reserve(bindings.size());
    for (auto& binding : bindings) {
      output.push_back({
        .source_id = std::move(binding.server_id),
        .tool_name = std::move(binding.tool_name),
        .exposed_name = std::move(binding.exposed_name),
      });
    }
    return output;
  }

  detail::bound_tool_provider provider_;
};

} // namespace wuwe::agent::mcp

#endif // WUWE_AGENT_MCP_SKILLS_ADAPTER_HPP
