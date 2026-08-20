#ifndef WUWE_AGENT_MCP_DETAIL_BOUND_TOOL_PROVIDER_HPP
#define WUWE_AGENT_MCP_DETAIL_BOUND_TOOL_PROVIDER_HPP

#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <wuwe/agent/capability/capability.hpp>
#include <wuwe/agent/tools/tool_contract.hpp>

namespace wuwe::agent::mcp::detail {

struct bound_tool_binding {
  std::string source_id;
  std::string tool_name;
  std::string exposed_name;
};

struct bound_tool_source {
  std::function<bool(const std::string&)> running;
  std::function<nlohmann::json(const std::string&)> list_tools;
  std::function<nlohmann::json(
    const std::string&, std::string, nlohmann::json)> call_tool;
};

struct bound_tool_profile {
  std::string label;
  std::string source_label;
  std::string separator { "__" };
  bool require_running_source { true };
  std::string capability_name;
  std::string capability_summary;
  std::function<void(const bound_tool_binding&, tools::tool_descriptor&)> configure_descriptor;
};

class bound_tool_provider final {
public:
  bound_tool_provider(
    bound_tool_source source, std::vector<bound_tool_binding> bindings, bound_tool_profile profile)
      : source_(std::move(source)), profile_(std::move(profile)) {
    if (!source_.running || !source_.list_tools || !source_.call_tool)
      throw std::invalid_argument("bound MCP tool provider requires complete source callbacks");
    if (profile_.label.empty() || profile_.source_label.empty() ||
        profile_.capability_name.empty() || profile_.capability_summary.empty()) {
      throw std::invalid_argument("bound MCP tool provider profile is incomplete");
    }
    std::map<std::string, nlohmann::json> discovery_cache;
    for (auto& binding : bindings)
      add_binding(std::move(binding), discovery_cache);
  }

  [[nodiscard]] std::vector<tools::tool_descriptor> descriptors() const {
    std::vector<tools::tool_descriptor> output;
    output.reserve(bindings_.size());
    for (const auto& [_, binding] : bindings_)
      output.push_back(binding.descriptor);
    return output;
  }

  [[nodiscard]] std::vector<llm_tool> tools() const {
    std::vector<llm_tool> output;
    output.reserve(bindings_.size());
    for (const auto& [_, binding] : bindings_)
      output.push_back(binding.descriptor.model_tool());
    return output;
  }

  [[nodiscard]] tools::tool_provider_capabilities contract_capabilities(
    std::string_view) const noexcept {
    // The synchronous MCP call boundary cannot guarantee invocation-context, heartbeat, or
    // cooperative cancellation propagation after dispatch.
    return {};
  }

  llm_tool_result invoke(const std::string& name, const std::string& arguments_json) const {
    return invoke({
      .name = name,
      .arguments_json = arguments_json,
      .descriptor = descriptor_for(name),
    });
  }

  llm_tool_result invoke(const tools::tool_invocation& invocation) const {
    const auto found = bindings_.find(invocation.name);
    if (found == bindings_.end()) {
      return failure(profile_.label + " tool is not bound: " + invocation.name,
        std::errc::function_not_supported,
        tools::tool_error_category::not_found);
    }
    if (invocation.stop_token.stop_requested()) {
      return failure(profile_.label + " tool call cancelled before dispatch",
        std::errc::operation_canceled,
        tools::tool_error_category::cancelled);
    }
    nlohmann::json arguments;
    try {
      arguments =
        nlohmann::json::parse(invocation.arguments_json.empty() ? "{}" : invocation.arguments_json);
    }
    catch (const nlohmann::json::exception& exception) {
      return failure("invalid " + profile_.label + " tool arguments: " + exception.what(),
        std::errc::invalid_argument,
        tools::tool_error_category::invalid_input);
    }
    try {
      return result_from_response(
        source_.call_tool(found->second.source_id, found->second.tool_name, arguments));
    }
    catch (const std::exception& exception) {
      return failure(profile_.label + " tool call failed: " + exception.what(),
        std::errc::host_unreachable,
        tools::tool_error_category::unavailable);
    }
  }

private:
  struct binding_entry {
    std::string source_id;
    std::string tool_name;
    tools::tool_descriptor descriptor;
  };

  void add_binding(bound_tool_binding binding,
    std::map<std::string, nlohmann::json>& discovery_cache) {
    if (binding.source_id.empty() || binding.tool_name.empty()) {
      throw std::invalid_argument(
        profile_.label + " binding requires " + profile_.source_label + " and tool names");
    }
    if (profile_.require_running_source && !source_.running(binding.source_id)) {
      throw std::invalid_argument(
        "bound " + profile_.source_label + " is not running: " + binding.source_id);
    }
    if (binding.exposed_name.empty())
      binding.exposed_name = binding.source_id + profile_.separator + binding.tool_name;
    if (bindings_.contains(binding.exposed_name)) {
      throw std::invalid_argument(
        "duplicate " + profile_.label + " tool binding: " + binding.exposed_name);
    }

    auto discovered = discovery_cache.find(binding.source_id);
    if (discovered == discovery_cache.end()) {
      const auto response = source_.list_tools(binding.source_id);
      if (!response.is_object()) {
        throw std::invalid_argument(
          "bound " + profile_.source_label + " returned an invalid tool list");
      }
      if (response.contains("error")) {
        throw std::invalid_argument(
          "bound " + profile_.source_label + " rejected tool discovery");
      }
      const auto result = response.value("result", nlohmann::json::object());
      if (!result.is_object()) {
        throw std::invalid_argument(
          "bound " + profile_.source_label + " returned an invalid tool list result");
      }
      auto listed = result.value("tools", nlohmann::json::array());
      if (!listed.is_array()) {
        throw std::invalid_argument(
          "bound " + profile_.source_label + " returned invalid tool descriptors");
      }
      discovered = discovery_cache.emplace(binding.source_id, std::move(listed)).first;
    }
    const nlohmann::json* selected = nullptr;
    for (const auto& item : discovered->second) {
      if (item.is_object() && item.value("name", std::string {}) == binding.tool_name) {
        selected = &item;
        break;
      }
    }
    if (!selected) {
      throw std::invalid_argument(
        profile_.label + " tool is not advertised by the " + profile_.source_label + ": " +
        binding.tool_name);
    }

    tools::tool_descriptor descriptor {
      .name = binding.exposed_name,
      .version = "mcp",
      .description = selected->value("description", std::string {}),
      .input_schema = selected->value("inputSchema", nlohmann::json::object()),
      .output_schema = selected->value("outputSchema", nlohmann::json::object()),
      .side_effect = tools::tool_side_effect::write,
      .idempotency = tools::tool_idempotency::unknown,
      .approval = tools::tool_approval_mode::policy,
      .capabilities = {
        {
          .name = profile_.capability_name,
          .risk = capability::capability_risk_level::high,
          .summary = profile_.capability_summary,
          .resources = { binding.source_id + ":" + binding.tool_name },
        },
      },
    };
    if (profile_.configure_descriptor)
      profile_.configure_descriptor(binding, descriptor);
    tools::validate_tool_descriptor(descriptor);
    bindings_.emplace(binding.exposed_name,
      binding_entry {
        .source_id = std::move(binding.source_id),
        .tool_name = std::move(binding.tool_name),
        .descriptor = std::move(descriptor),
      });
  }

  [[nodiscard]] tools::tool_descriptor descriptor_for(const std::string& name) const {
    const auto found = bindings_.find(name);
    return found == bindings_.end() ? tools::tool_descriptor { .name = name }
                                    : found->second.descriptor;
  }

  static llm_tool_result result_from_response(const nlohmann::json& response) {
    if (!response.is_object() || response.contains("error")) {
      return failure("MCP tool call returned a protocol error",
        std::errc::protocol_error,
        tools::tool_error_category::internal);
    }
    try {
      const auto result = response.value("result", nlohmann::json::object());
      if (!result.is_object()) {
        return failure("MCP tool call returned an invalid result",
          std::errc::protocol_error,
          tools::tool_error_category::internal);
      }
      std::string content;
      const auto items = result.value("content", nlohmann::json::array());
      if (!items.is_array()) {
        return failure("MCP tool call returned invalid content",
          std::errc::protocol_error,
          tools::tool_error_category::internal);
      }
      for (const auto& item : items) {
        if (!content.empty())
          content.push_back('\n');
        if (item.is_object() && item.value("type", std::string {}) == "text")
          content += item.value("text", std::string {});
        else
          content += item.dump();
      }
      if (result.value("isError", false)) {
        return {
          .content = std::move(content),
          .error_code = std::make_error_code(std::errc::io_error),
          .error_category = tools::tool_error_category::internal,
        };
      }
      return { .content = std::move(content) };
    }
    catch (const nlohmann::json::exception&) {
      return failure("MCP tool call returned malformed content",
        std::errc::protocol_error,
        tools::tool_error_category::internal);
    }
  }

  static llm_tool_result failure(
    std::string message, std::errc code, tools::tool_error_category category) {
    return {
      .content = std::move(message),
      .error_code = std::make_error_code(code),
      .error_category = category,
    };
  }

  bound_tool_source source_;
  bound_tool_profile profile_;
  std::map<std::string, binding_entry> bindings_;
};

} // namespace wuwe::agent::mcp::detail

#endif // WUWE_AGENT_MCP_DETAIL_BOUND_TOOL_PROVIDER_HPP
