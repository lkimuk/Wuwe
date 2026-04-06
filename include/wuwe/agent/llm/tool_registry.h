#ifndef WUWE_AGENT_LLM_TOOL_REGISTRY_H
#define WUWE_AGENT_LLM_TOOL_REGISTRY_H

#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <wuwe/agent/llm/llm_types.h>
#include <wuwe/common/wuwe_fwd.h>

WUWE_NAMESPACE_BEGIN

struct llm_tool_result {
  std::string content;
  std::error_code error_code;
};

using llm_tool_handler = std::function<llm_tool_result(const std::string& arguments_json)>;

class llm_tool_registry {
public:
  void register_tool(llm_tool tool, llm_tool_handler handler) {
    const std::string key = tool.name;
    tools_[key] = std::move(tool);
    handlers_[key] = std::move(handler);
  }

  std::vector<llm_tool> tools() const {
    std::vector<llm_tool> all;
    all.reserve(tools_.size());
    for (const auto& [_, tool] : tools_) {
      all.push_back(tool);
    }
    return all;
  }

  llm_tool_result invoke(const std::string& name, const std::string& arguments_json) const {
    const auto it = handlers_.find(name);
    if (it == handlers_.end()) {
      return {
        .content = "tool not found: " + name,
        .error_code = std::make_error_code(std::errc::function_not_supported)
      };
    }
    return it->second(arguments_json);
  }

private:
  std::unordered_map<std::string, llm_tool> tools_;
  std::unordered_map<std::string, llm_tool_handler> handlers_;
};

WUWE_NAMESPACE_END

#endif // WUWE_AGENT_LLM_TOOL_REGISTRY_H
