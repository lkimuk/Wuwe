#ifndef WUWE_AGENT_CORE_TEXT_SEARCH_HPP
#define WUWE_AGENT_CORE_TEXT_SEARCH_HPP

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace wuwe::agent::text {

inline std::string lowercase_ascii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

class ascii_token_set {
public:
  ascii_token_set() = default;

  explicit ascii_token_set(std::string_view text) {
    std::string current;
    for (const unsigned char c : text) {
      if (std::isalnum(c)) {
        current.push_back(static_cast<char>(std::tolower(c)));
      }
      else {
        flush(current);
      }
    }
    flush(current);
  }

  bool empty() const {
    return tokens_.empty();
  }

  std::size_t size() const {
    return tokens_.size();
  }

  bool contains(const std::string& token) const {
    return tokens_.contains(token);
  }

  std::size_t count_matches(const ascii_token_set& other) const {
    std::size_t matches = 0;
    for (const auto& token : tokens_) {
      if (other.contains(token)) {
        ++matches;
      }
    }
    return matches;
  }

private:
  void flush(std::string& current) {
    if (!current.empty()) {
      tokens_.insert(std::move(current));
      current.clear();
    }
  }

  std::unordered_set<std::string> tokens_;
};

inline bool contains_ascii_case_insensitive(std::string_view haystack, std::string_view needle) {
  if (needle.empty()) {
    return true;
  }
  return lowercase_ascii(std::string(haystack)).find(lowercase_ascii(std::string(needle))) !=
         std::string::npos;
}

inline double token_overlap_ratio(std::string_view query, std::string_view content) {
  const ascii_token_set query_tokens(query);
  if (query_tokens.empty()) {
    return 0.0;
  }
  const ascii_token_set content_tokens(content);
  return static_cast<double>(query_tokens.count_matches(content_tokens)) /
         static_cast<double>(query_tokens.size());
}

} // namespace wuwe::agent::text

#endif // WUWE_AGENT_CORE_TEXT_SEARCH_HPP
