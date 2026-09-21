#ifndef WUWE_INTERNAL_LLM_ENDPOINT_HPP
#define WUWE_INTERNAL_LLM_ENDPOINT_HPP

#include <algorithm>
#include <string>
#include <string_view>

namespace wuwe::agent::llm::detail {

inline std::string_view endpoint_base_path(std::string_view base) {
  const auto scheme = base.find("://");
  const auto start =
    scheme == std::string_view::npos ? std::string_view::npos : base.find('/', scheme + 3);
  return start == std::string_view::npos ? std::string_view {} : base.substr(start);
}

inline std::string api_endpoint(std::string base, std::string_view prefix, std::string_view route) {
  while (!base.empty() && base.back() == '/') {
    base.pop_back();
  }
  if (!endpoint_base_path(base).ends_with(prefix)) {
    base += prefix;
  }
  base += route;
  return base;
}

// Shared by generation and discovery so a model-list query cannot succeed at
// one API root while generation accidentally duplicates its version prefix.
inline std::string openai_chat_endpoint(std::string base, std::string path) {
  if (path.empty()) {
    path = "/v1/chat/completions";
  }
  else if (path.front() != '/') {
    path.insert(path.begin(), '/');
  }
  while (!base.empty() && base.back() == '/') {
    base.pop_back();
  }
  constexpr std::string_view chat_suffix = "/chat/completions";
  const auto base_path = endpoint_base_path(base);
  if (path.ends_with(chat_suffix) && !base_path.empty()) {
    const auto prefix = std::string_view { path }.substr(0, path.size() - chat_suffix.size());
    const auto last_segment = base_path.substr(base_path.rfind('/') + 1);
    const bool versioned = last_segment.size() > 1 && last_segment.front() == 'v' &&
                           std::all_of(last_segment.begin() + 1, last_segment.end(), [](char c) {
                             return c >= '0' && c <= '9';
                           });
    if ((!prefix.empty() && base_path.ends_with(prefix)) || (prefix == "/v1" && versioned)) {
      path.erase(0, prefix.size());
    }
  }
  return base + path;
}

} // namespace wuwe::agent::llm::detail

#endif // WUWE_INTERNAL_LLM_ENDPOINT_HPP
