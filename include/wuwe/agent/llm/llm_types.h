#ifndef WUWE_AGENT_LLM_TYPES_H
#define WUWE_AGENT_LLM_TYPES_H

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
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
  std::string stop_reason;
  std::vector<llm_tool_call> tool_calls;
  std::map<std::string, std::string> metadata;

  explicit operator bool() const noexcept {
    return !error_code;
  }
};

enum class llm_stream_event_type {
  content_delta,
  tool_call_delta,
  tool_call_done,
  done,
  error
};

struct llm_tool_call_delta {
  int index { 0 };
  std::string id;
  std::string name_delta;
  std::string arguments_delta;
};

struct llm_stream_event {
  llm_stream_event_type type { llm_stream_event_type::content_delta };
  std::string content_delta;
  std::optional<llm_tool_call_delta> tool_call_delta;
  std::optional<llm_tool_call> tool_call;
  std::optional<llm_response> response;
  std::error_code error_code;
  std::string message;
};

struct llm_stream_callbacks {
  std::function<void(const llm_stream_event&)> on_event;
};

WUWE_NAMESPACE_END

#endif // WUWE_AGENT_LLM_TYPES_H
