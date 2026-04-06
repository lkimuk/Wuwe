#ifndef WUWE_AGENT_LLM_TYPES_H
#define WUWE_AGENT_LLM_TYPES_H

#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include <wuwe/common/wuwe_fwd.h>

WUWE_NAMESPACE_BEGIN

struct llm_tool {
  std::string name;
  std::string description;
  std::string parameters_json_schema { "{}" };
};

enum class llm_tool_choice_mode {
  auto_,
  none,
  required,
  named
};

struct llm_tool_choice {
  llm_tool_choice_mode mode { llm_tool_choice_mode::auto_ };
  std::string name;
};

struct llm_tool_call {
  std::string id;
  std::string name;
  std::string arguments_json;
};

struct chat_message {
  std::string role;
  std::string content;
  std::optional<std::string> name;
  std::optional<std::string> tool_call_id;
  std::vector<llm_tool_call> tool_calls;
};

struct llm_request {
  std::string model;
  std::vector<chat_message> messages;
  double temperature { 0.2 };
  std::optional<std::string> response_format;
  std::vector<llm_tool> tools;
  std::optional<llm_tool_choice> tool_choice;
};

struct llm_usage {
  int prompt_tokens { 0 };
  int completion_tokens { 0 };
  int total_tokens { 0 };
};

struct llm_response {
  std::string content;
  std::error_code error_code;
  llm_usage usage;
  std::string finish_reason;
  std::vector<llm_tool_call> tool_calls;

  explicit operator bool() const noexcept {
    return !error_code;
  }
};

WUWE_NAMESPACE_END

#endif // WUWE_AGENT_LLM_TYPES_H
