#include <wuwe/runtime_host/plugin_tools.hpp>

#include <algorithm>
#include <map>
#include <stdexcept>
#include <utility>

#include <wuwe/agent/mcp/detail/bound_tool_provider.hpp>
#include <wuwe/runtime_host/plugin_runtime.hpp>

namespace wuwe::runtime_host {
namespace {

std::vector<agent::mcp::detail::bound_tool_binding> bindings_from(
  std::vector<plugin_tool_binding> bindings) {
  std::vector<agent::mcp::detail::bound_tool_binding> output;
  output.reserve(bindings.size());
  for (auto& binding : bindings) {
    output.push_back({
      .source_id = std::move(binding.plugin_id),
      .tool_name = std::move(binding.tool_name),
      .exposed_name = std::move(binding.exposed_name),
    });
  }
  return output;
}

struct bound_plugin_identity {
  std::string version;
  std::string content_sha256;
};

std::map<std::string, bound_plugin_identity> identities_from(
  plugin_runtime& runtime, const std::vector<plugin_tool_binding>& bindings) {
  const auto catalog = runtime.catalog_snapshot();
  std::map<std::string, bound_plugin_identity> output;
  for (const auto& binding : bindings) {
    if (binding.plugin_id.empty() || output.contains(binding.plugin_id))
      continue;
    const auto package = catalog.find(binding.plugin_id);
    if (!package)
      throw std::invalid_argument("Runtime plugin is not registered: " + binding.plugin_id);
    output.emplace(binding.plugin_id,
      bound_plugin_identity {
        .version = package->manifest().version,
        .content_sha256 = package->content_sha256(),
      });
  }
  return output;
}

} // namespace

struct plugin_tool_provider::implementation {
  implementation(plugin_runtime& runtime, std::vector<plugin_tool_binding> bindings,
    plugin_tool_provider_options options)
      : runtime_(&runtime), identities_(identities_from(runtime, bindings)), provider(
          {
            .running =
              [this](const std::string& id) { return active_identity_matches(id); },
            .list_tools =
              [this](const std::string& id) {
                return nlohmann::json { { "result",
                  plugin_tool_provider::list_tools_for_package(
                    *runtime_, id, identities_.at(id).content_sha256) } };
              },
            .call_tool =
              [this](const std::string& id, std::string name, nlohmann::json arguments) {
                return nlohmann::json { { "result",
                  plugin_tool_provider::call_tool_for_package(*runtime_,
                    id,
                    identities_.at(id).content_sha256,
                    std::move(name),
                    std::move(arguments)) } };
              },
          },
          bindings_from(std::move(bindings)),
          {
            .label = "Runtime plugin",
            .source_label = "plugin",
            .separator = std::move(options.separator),
            .require_running_source = true,
            .capability_name = "runtime.plugin.tool.call",
            .capability_summary = "Invoke an explicitly bound Runtime plugin tool",
            .configure_descriptor =
              [this](const agent::mcp::detail::bound_tool_binding& binding,
                agent::tools::tool_descriptor& descriptor) {
                const auto& identity = identities_.at(binding.source_id);
                descriptor.version = identity.version;
                descriptor.metadata = {
                  { "wuwe.runtime.plugin.id", binding.source_id },
                  { "wuwe.runtime.plugin.version", identity.version },
                  { "wuwe.runtime.plugin.content_sha256", identity.content_sha256 },
                  { "wuwe.runtime.plugin.tool", binding.tool_name },
                };
              },
          }) {
  }

  [[nodiscard]] bool active_identity_matches(const std::string& id) const {
    const auto expected = identities_.find(id);
    if (expected == identities_.end())
      return false;
    const auto snapshots = runtime_->process_snapshots();
    return std::any_of(snapshots.begin(), snapshots.end(), [&](const auto& snapshot) {
      return snapshot.id == id && snapshot.state == plugin_process_state::running &&
             snapshot.content_sha256 == expected->second.content_sha256;
    });
  }

  plugin_runtime* runtime_;
  std::map<std::string, bound_plugin_identity> identities_;
  agent::mcp::detail::bound_tool_provider provider;
};

plugin_tool_provider::plugin_tool_provider(plugin_runtime& runtime,
  std::vector<plugin_tool_binding> bindings, plugin_tool_provider_options options)
    : impl_(
        std::make_unique<implementation>(runtime, std::move(bindings), std::move(options))) {
}

plugin_tool_provider::~plugin_tool_provider() = default;

plugin_tool_provider::plugin_tool_provider(plugin_tool_provider&&) noexcept = default;

plugin_tool_provider& plugin_tool_provider::operator=(plugin_tool_provider&&) noexcept = default;

nlohmann::json plugin_tool_provider::list_tools_for_package(
  plugin_runtime& runtime, const std::string& id, std::string_view expected_content_sha256) {
  return runtime.list_tools_for_package(id, expected_content_sha256);
}

nlohmann::json plugin_tool_provider::call_tool_for_package(plugin_runtime& runtime,
  const std::string& id, std::string_view expected_content_sha256, std::string name,
  nlohmann::json arguments) {
  return runtime.call_tool_for_package(
    id, expected_content_sha256, std::move(name), std::move(arguments));
}

std::vector<agent::tools::tool_descriptor> plugin_tool_provider::descriptors() const {
  return impl_->provider.descriptors();
}

std::vector<llm_tool> plugin_tool_provider::tools() const {
  return impl_->provider.tools();
}

agent::tools::tool_provider_capabilities plugin_tool_provider::contract_capabilities(
  std::string_view name) const noexcept {
  return impl_->provider.contract_capabilities(name);
}

llm_tool_result plugin_tool_provider::invoke(
  const std::string& name, const std::string& arguments_json) const {
  return impl_->provider.invoke(name, arguments_json);
}

llm_tool_result plugin_tool_provider::invoke(
  const agent::tools::tool_invocation& invocation) const {
  return impl_->provider.invoke(invocation);
}

} // namespace wuwe::runtime_host
