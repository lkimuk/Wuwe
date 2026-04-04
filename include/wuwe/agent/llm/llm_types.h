#ifndef WUWE_AGENT_LLM_TYPES_H
#define WUWE_AGENT_LLM_TYPES_H

#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include <wuwe/common/wuwe_fwd.h>

WUWE_NAMESPACE_BEGIN

struct chat_message {
  std::string role;
  std::string content;
};

struct llm_request {
  std::string model;
  std::vector<chat_message> messages;
  double temperature { 0.2 };
  std::optional<std::string> response_format;
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

  explicit operator bool() const noexcept {
    return !error_code;
  }
};

WUWE_NAMESPACE_END

#endif // WUWE_AGENT_LLM_TYPES_H
