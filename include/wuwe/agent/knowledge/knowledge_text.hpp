#ifndef WUWE_AGENT_KNOWLEDGE_TEXT_HPP
#define WUWE_AGENT_KNOWLEDGE_TEXT_HPP

#include <cctype>
#include <string>
#include <string_view>

namespace wuwe::agent::knowledge::text_detail {

inline std::string lowercase_ascii(std::string_view text) {
  std::string result;
  result.reserve(text.size());
  for (const auto ch : text) {
    result.push_back(static_cast<char>(
      std::tolower(static_cast<unsigned char>(ch))));
  }
  return result;
}

inline std::string trim_copy(std::string_view value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
    value.remove_prefix(1);
  }
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
    value.remove_suffix(1);
  }
  return std::string(value);
}

inline std::size_t find_case_insensitive(
  const std::string& text,
  std::string_view needle,
  std::size_t start = 0) {
  if (needle.empty() || needle.size() > text.size()) {
    return std::string::npos;
  }
  for (std::size_t index = start; index + needle.size() <= text.size(); ++index) {
    bool matched = true;
    for (std::size_t offset = 0; offset < needle.size(); ++offset) {
      if (std::tolower(static_cast<unsigned char>(text[index + offset])) !=
          std::tolower(static_cast<unsigned char>(needle[offset]))) {
        matched = false;
        break;
      }
    }
    if (matched) {
      return index;
    }
  }
  return std::string::npos;
}

inline void replace_all(std::string& text, std::string_view from, std::string_view to) {
  if (from.empty()) {
    return;
  }
  std::size_t pos = 0;
  while ((pos = text.find(from, pos)) != std::string::npos) {
    text.replace(pos, from.size(), to);
    pos += to.size();
  }
}

inline std::string decode_common_html_entities(std::string text) {
  replace_all(text, "&nbsp;", " ");
  replace_all(text, "&emsp;", " ");
  replace_all(text, "&ensp;", " ");
  replace_all(text, "&amp;", "&");
  replace_all(text, "&lt;", "<");
  replace_all(text, "&gt;", ">");
  replace_all(text, "&quot;", "\"");
  replace_all(text, "&#39;", "'");
  replace_all(text, "&apos;", "'");
  replace_all(text, "&copy;", "(c)");
  replace_all(text, "&mdash;", "-");
  replace_all(text, "&ndash;", "-");
  replace_all(text, "&bull;", "*");
  replace_all(text, "&ldquo;", "\"");
  replace_all(text, "&rdquo;", "\"");
  replace_all(text, "&lsquo;", "'");
  replace_all(text, "&rsquo;", "'");
  return text;
}

} // namespace wuwe::agent::knowledge::text_detail

#endif // WUWE_AGENT_KNOWLEDGE_TEXT_HPP
