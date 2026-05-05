#ifndef WUWE_AGENT_KNOWLEDGE_GROUNDING_HPP
#define WUWE_AGENT_KNOWLEDGE_GROUNDING_HPP

#include <cctype>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <wuwe/agent/knowledge/knowledge_record.hpp>

namespace wuwe::agent::knowledge {

struct knowledge_grounding_policy {
  bool require_at_least_one_citation { true };
  bool require_sentence_citations { false };
};

struct knowledge_grounding_report {
  bool grounded { true };
  std::vector<int> citation_numbers;
  std::vector<int> invalid_citation_numbers;
  std::vector<std::string> unsupported_sentences;
};

class knowledge_grounding_checker {
public:
  explicit knowledge_grounding_checker(knowledge_grounding_policy policy = {})
      : policy_(policy) {
  }

  knowledge_grounding_report check(
    std::string_view answer,
    const std::vector<knowledge_result>& cited_results) const {
    knowledge_grounding_report report;
    report.citation_numbers = extract_citations(answer);

    const auto max_citation = static_cast<int>(cited_results.size());
    for (const auto citation : report.citation_numbers) {
      if (citation <= 0 || citation > max_citation) {
        report.invalid_citation_numbers.push_back(citation);
      }
    }

    if (policy_.require_at_least_one_citation && report.citation_numbers.empty()) {
      report.grounded = false;
    }
    if (!report.invalid_citation_numbers.empty()) {
      report.grounded = false;
    }

    if (policy_.require_sentence_citations) {
      for (const auto& sentence : split_sentences(answer)) {
        if (!sentence.empty() && extract_citations(sentence).empty()) {
          report.unsupported_sentences.push_back(sentence);
        }
      }
      if (!report.unsupported_sentences.empty()) {
        report.grounded = false;
      }
    }

    return report;
  }

private:
  static std::vector<int> extract_citations(std::string_view text) {
    std::set<int> unique;
    for (std::size_t index = 0; index < text.size(); ++index) {
      if (text[index] != '[') {
        continue;
      }
      std::size_t cursor = index + 1;
      int value = 0;
      bool saw_digit = false;
      while (cursor < text.size() && std::isdigit(static_cast<unsigned char>(text[cursor]))) {
        saw_digit = true;
        value = value * 10 + (text[cursor] - '0');
        ++cursor;
      }
      if (saw_digit && cursor < text.size() && text[cursor] == ']') {
        unique.insert(value);
      }
    }
    return { unique.begin(), unique.end() };
  }

  static std::vector<std::string> split_sentences(std::string_view text) {
    std::vector<std::string> sentences;
    std::string current;
    for (const auto ch : text) {
      current.push_back(ch);
      if (ch == '.' || ch == '!' || ch == '?') {
        trim(current);
        if (!current.empty()) {
          sentences.push_back(std::move(current));
          current.clear();
        }
      }
    }
    trim(current);
    if (!current.empty()) {
      sentences.push_back(std::move(current));
    }
    return sentences;
  }

  static void trim(std::string& value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
      value.erase(value.begin());
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
      value.pop_back();
    }
  }

  knowledge_grounding_policy policy_;
};

} // namespace wuwe::agent::knowledge

#endif // WUWE_AGENT_KNOWLEDGE_GROUNDING_HPP
