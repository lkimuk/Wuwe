#ifndef WUWE_AGENT_EXECUTION_EXECUTION_TOOLS_HPP
#define WUWE_AGENT_EXECUTION_EXECUTION_TOOLS_HPP

#include <optional>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <wuwe/agent/execution/execution_runtime.hpp>
#include <wuwe/agent/llm/llm_types.h>
#include <wuwe/agent/tools/tool.hpp>

namespace wuwe::agent::execution {

struct run_python_snippet {
  static constexpr std::string_view description =
    "Run a short Python snippet with bounded output and timeout.";

  std::string code;
  std::optional<std::string> stdin_text;
  std::optional<int> timeout_ms;
};

struct execution_tool_options {
  std::string tool_name { "run_python_snippet" };
  std::string description {
    "Run a short Python snippet with bounded output and timeout."
  };
  std::size_t max_arguments_bytes { 131072 };
  bool allow_empty_stdin { true };
  bool allow_additional_arguments { false };
  bool reject_timeout_outside_limits { true };
  std::optional<int> min_timeout_ms { 1 };
  std::optional<int> max_timeout_ms;
  std::optional<int> default_timeout_ms;
};

class execution_tool_provider {
public:
  execution_tool_provider(
    execution_runtime& runtime,
    execution_tool_options options = {});

  [[nodiscard]] std::vector<llm_tool> tools() const;

  [[nodiscard]] llm_tool_result invoke(
    const std::string& name,
    const std::string& arguments_json) const;

  [[nodiscard]] llm_tool_result invoke(
    const std::string& name,
    const std::string& arguments_json,
    std::stop_token stop_token) const;

private:
  execution_runtime& runtime_;
  execution_tool_options options_;
};

} // namespace wuwe::agent::execution

#endif // WUWE_AGENT_EXECUTION_EXECUTION_TOOLS_HPP
