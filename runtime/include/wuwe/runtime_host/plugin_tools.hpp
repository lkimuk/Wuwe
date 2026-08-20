#ifndef WUWE_RUNTIME_HOST_PLUGIN_TOOLS_HPP
#define WUWE_RUNTIME_HOST_PLUGIN_TOOLS_HPP

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <wuwe/agent/tools/tool_contract.hpp>

namespace wuwe::runtime_host {

class plugin_runtime;

struct plugin_tool_binding {
  std::string plugin_id;
  std::string tool_name;
  std::string exposed_name;
};

struct plugin_tool_provider_options {
  std::string separator { "__" };
};

// Exposes only explicitly selected process-plugin tools through Wuwe's governed
// Tool Contract. Bound plugins must be active so their MCP schemas can be
// discovered. The referenced plugin_runtime must outlive the provider.
class plugin_tool_provider final {
public:
  plugin_tool_provider(plugin_runtime& runtime, std::vector<plugin_tool_binding> bindings,
    plugin_tool_provider_options options = {});
  ~plugin_tool_provider();

  plugin_tool_provider(const plugin_tool_provider&) = delete;
  plugin_tool_provider& operator=(const plugin_tool_provider&) = delete;
  plugin_tool_provider(plugin_tool_provider&&) noexcept;
  plugin_tool_provider& operator=(plugin_tool_provider&&) noexcept;

  [[nodiscard]] std::vector<agent::tools::tool_descriptor> descriptors() const;
  [[nodiscard]] std::vector<llm_tool> tools() const;
  [[nodiscard]] agent::tools::tool_provider_capabilities contract_capabilities(
    std::string_view name) const noexcept;

  llm_tool_result invoke(const std::string& name, const std::string& arguments_json) const;
  llm_tool_result invoke(const agent::tools::tool_invocation& invocation) const;

private:
  static nlohmann::json list_tools_for_package(
    plugin_runtime& runtime, const std::string& id, std::string_view expected_content_sha256);
  static nlohmann::json call_tool_for_package(plugin_runtime& runtime, const std::string& id,
    std::string_view expected_content_sha256, std::string name, nlohmann::json arguments);

  struct implementation;
  std::unique_ptr<implementation> impl_;
};

} // namespace wuwe::runtime_host

#endif // WUWE_RUNTIME_HOST_PLUGIN_TOOLS_HPP
