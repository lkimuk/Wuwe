#ifndef WUWE_AGENT_LLM_TYPES_H
#define WUWE_AGENT_LLM_TYPES_H

#include <cctype>
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

enum class llm_reasoning_language_control {
  unsupported,
  prompt_contract,
  provider_native,
  verified_reliable,
};

inline std::string_view to_string(llm_reasoning_language_control control) noexcept {
  switch (control) {
    case llm_reasoning_language_control::unsupported:
      return "unsupported";
    case llm_reasoning_language_control::prompt_contract:
      return "prompt_contract";
    case llm_reasoning_language_control::provider_native:
      return "provider_native";
    case llm_reasoning_language_control::verified_reliable:
      return "verified_reliable";
  }
  return "unsupported";
}

struct llm_language_preferences {
  std::string response_language;
  std::string reasoning_language;
  std::string locale;
};

inline std::string effective_response_language(const llm_language_preferences& preferences) {
  if (!preferences.response_language.empty()) {
    return preferences.response_language;
  }
  return preferences.locale;
}

inline std::string effective_reasoning_language(const llm_language_preferences& preferences) {
  if (!preferences.reasoning_language.empty()) {
    return preferences.reasoning_language;
  }
  if (!preferences.response_language.empty()) {
    return preferences.response_language;
  }
  return preferences.locale;
}

inline bool has_language_preferences(const llm_language_preferences& preferences) {
  return !preferences.response_language.empty() ||
         !preferences.reasoning_language.empty() ||
         !preferences.locale.empty();
}

inline std::string llm_language_contract(const llm_language_preferences& preferences) {
  const auto response_language = effective_response_language(preferences);
  const auto reasoning_language = effective_reasoning_language(preferences);
  if (response_language.empty() && reasoning_language.empty() && preferences.locale.empty()) {
    return {};
  }

  std::string contract = "Language contract:\n";
  if (!preferences.locale.empty()) {
    contract += "- User locale: " + preferences.locale + ".\n";
  }
  if (!response_language.empty()) {
    contract += "- Write the final answer content in " + response_language + ".\n";
  }
  if (!reasoning_language.empty()) {
    contract += "- If provider-visible reasoning, thinking, or reasoning summary fields "
                "are returned, write those visible summaries in " +
                reasoning_language + ".\n";
  }
  contract += "- Keep tool names, code identifiers, file paths, API names, command names, "
              "schema keys, and quoted source text unchanged.\n";
  contract += "- Do not expose hidden chain-of-thought; only use provider-supported visible "
              "reasoning summaries when available.";
  return contract;
}

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
  llm_language_preferences language;
};

struct llm_usage {
  int prompt_tokens { 0 };
  int completion_tokens { 0 };
  int total_tokens { 0 };
};

struct llm_response {
  std::string content;
  std::string reasoning_summary;
  std::error_code error_code;
  llm_usage usage;
  std::string finish_reason;
  std::string stop_reason;
  std::vector<llm_tool_call> tool_calls;
  std::map<std::string, std::string> metadata;
  std::map<std::string, std::string> reasoning_metadata;

  explicit operator bool() const noexcept {
    return !error_code;
  }
};

enum class llm_stream_event_type {
  content_delta,
  reasoning_delta,
  reasoning_done,
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
  std::string reasoning_delta;
  std::string reasoning_summary;
  std::map<std::string, std::string> reasoning_metadata;
  std::optional<llm_tool_call_delta> tool_call_delta;
  std::optional<llm_tool_call> tool_call;
  std::optional<llm_response> response;
  std::error_code error_code;
  std::string message;
};

struct llm_stream_callbacks {
  std::function<void(const llm_stream_event&)> on_event;
  std::function<void(std::string_view)> on_reasoning_delta;
  std::function<void(std::string_view)> on_reasoning_done;
};

inline std::string primary_language_subtag(std::string_view language) {
  std::string output;
  for (char ch : language) {
    if (ch == '-' || ch == '_') {
      break;
    }
    output.push_back(static_cast<char>(
      std::tolower(static_cast<unsigned char>(ch))));
  }
  return output;
}

inline bool utf8_next_codepoint(std::string_view text, std::size_t& index, unsigned int& codepoint) {
  const auto first = static_cast<unsigned char>(text[index]);
  if (first < 0x80) {
    codepoint = first;
    ++index;
    return true;
  }

  int length = 0;
  unsigned int value = 0;
  if ((first & 0xe0) == 0xc0) {
    length = 2;
    value = first & 0x1f;
  }
  else if ((first & 0xf0) == 0xe0) {
    length = 3;
    value = first & 0x0f;
  }
  else if ((first & 0xf8) == 0xf0) {
    length = 4;
    value = first & 0x07;
  }
  else {
    ++index;
    return false;
  }

  if (index + static_cast<std::size_t>(length) > text.size()) {
    index = text.size();
    return false;
  }
  for (int offset = 1; offset < length; ++offset) {
    const auto byte = static_cast<unsigned char>(text[index + offset]);
    if ((byte & 0xc0) != 0x80) {
      ++index;
      return false;
    }
    value = (value << 6) | (byte & 0x3f);
  }
  index += static_cast<std::size_t>(length);
  codepoint = value;
  return true;
}

inline bool is_cjk_codepoint(unsigned int codepoint) {
  return (codepoint >= 0x3400 && codepoint <= 0x4dbf) ||
         (codepoint >= 0x4e00 && codepoint <= 0x9fff) ||
         (codepoint >= 0xf900 && codepoint <= 0xfaff) ||
         (codepoint >= 0x20000 && codepoint <= 0x2ebef);
}

inline std::string detect_reasoning_language(std::string_view text) {
  int ascii_letters = 0;
  int ascii_words = 0;
  bool in_ascii_word = false;
  bool has_cjk = false;

  for (std::size_t index = 0; index < text.size();) {
    unsigned int codepoint = 0;
    const auto before = index;
    if (!utf8_next_codepoint(text, index, codepoint)) {
      in_ascii_word = false;
      if (index == before) {
        ++index;
      }
      continue;
    }
    if (is_cjk_codepoint(codepoint)) {
      has_cjk = true;
    }
    if (codepoint < 0x80 &&
        std::isalpha(static_cast<unsigned char>(codepoint))) {
      ++ascii_letters;
      if (!in_ascii_word) {
        ++ascii_words;
        in_ascii_word = true;
      }
    }
    else {
      in_ascii_word = false;
    }
  }

  if (has_cjk) {
    return "zh";
  }
  if (ascii_letters >= 8 || ascii_words >= 2) {
    return "en";
  }
  return {};
}

inline bool language_tags_match(std::string_view requested, std::string_view detected) {
  const auto requested_primary = primary_language_subtag(requested);
  const auto detected_primary = primary_language_subtag(detected);
  return !requested_primary.empty() && requested_primary == detected_primary;
}

inline std::map<std::string, std::string> make_reasoning_language_metadata(
  const llm_language_preferences& preferences,
  llm_reasoning_language_control control,
  std::string_view sample) {
  std::map<std::string, std::string> metadata;
  const auto response_language = effective_response_language(preferences);
  const auto reasoning_language = effective_reasoning_language(preferences);
  if (!reasoning_language.empty()) {
    metadata["requested_language"] = reasoning_language;
  }
  if (!response_language.empty()) {
    metadata["response_language"] = response_language;
  }
  if (!preferences.locale.empty()) {
    metadata["locale"] = preferences.locale;
  }
  metadata["language_control"] = std::string(to_string(control));

  const auto detected_language = detect_reasoning_language(sample);
  if (detected_language.empty()) {
    metadata["language_detection"] = "unavailable";
  }
  else {
    metadata["language_detection"] = "heuristic";
    metadata["detected_language"] = detected_language;
    if (!reasoning_language.empty()) {
      metadata["language_mismatch"] =
        language_tags_match(reasoning_language, detected_language) ? "false" : "true";
    }
  }
  return metadata;
}

inline void merge_reasoning_language_metadata(
  std::map<std::string, std::string>& destination,
  const llm_language_preferences& preferences,
  llm_reasoning_language_control control,
  std::string_view sample) {
  for (const auto& [key, value] :
       make_reasoning_language_metadata(preferences, control, sample)) {
    destination[key] = value;
  }
}

inline void apply_reasoning_language_metadata(
  llm_response& response,
  const llm_language_preferences& preferences,
  llm_reasoning_language_control control) {
  if (response.reasoning_summary.empty()) {
    return;
  }
  merge_reasoning_language_metadata(
    response.reasoning_metadata,
    preferences,
    control,
    response.reasoning_summary);
}

WUWE_NAMESPACE_END

#endif // WUWE_AGENT_LLM_TYPES_H
