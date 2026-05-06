#ifndef WUWE_AGENT_KNOWLEDGE_EVAL_HPP
#define WUWE_AGENT_KNOWLEDGE_EVAL_HPP

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <wuwe/agent/knowledge/knowledge_retriever.hpp>

namespace wuwe::agent::knowledge {

struct knowledge_eval_case {
  std::string name;
  std::string query;
  std::vector<std::string> expected_document_ids;
  std::vector<std::string> expected_terms;
  std::map<std::string, std::string> filters;
  std::size_t limit { 6 };
};

struct knowledge_eval_case_result {
  std::string name;
  std::string query;
  bool hit {};
  bool terms_hit { true };
  double reciprocal_rank {};
  std::vector<std::string> returned_document_ids;
  std::vector<std::string> missing_terms;
};

struct knowledge_eval_result {
  std::size_t total {};
  std::size_t hits {};
  std::size_t term_hits {};
  double recall_at_k {};
  double term_recall {};
  double mean_reciprocal_rank {};
  std::vector<knowledge_eval_case_result> cases;
};

inline std::vector<knowledge_eval_case> load_knowledge_eval_cases(
  const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("failed to open knowledge eval cases: " + path.string());
  }

  nlohmann::json data;
  input >> data;
  if (!data.is_array()) {
    throw std::runtime_error("knowledge eval cases must be a JSON array");
  }

  std::vector<knowledge_eval_case> cases;
  cases.reserve(data.size());
  for (const auto& item : data) {
    if (!item.is_object()) {
      continue;
    }

    knowledge_eval_case eval_case;
    eval_case.name = item.value("name", std::string {});
    eval_case.query = item.value("query", std::string {});
    eval_case.limit = item.value("limit", eval_case.limit);
    if (item.contains("expected_document_ids")) {
      eval_case.expected_document_ids =
        item["expected_document_ids"].get<std::vector<std::string>>();
    }
    if (item.contains("expected_terms")) {
      eval_case.expected_terms = item["expected_terms"].get<std::vector<std::string>>();
    }
    if (item.contains("filters")) {
      eval_case.filters = item["filters"].get<std::map<std::string, std::string>>();
    }
    if (!eval_case.query.empty()) {
      cases.push_back(std::move(eval_case));
    }
  }
  return cases;
}

inline std::string normalize_eval_text(std::string_view text) {
  std::string output;
  output.reserve(text.size());
  bool previous_space = false;
  for (const auto ch : text) {
    const auto byte = static_cast<unsigned char>(ch);
    if (std::isspace(byte)) {
      if (!previous_space) {
        output.push_back(' ');
        previous_space = true;
      }
      continue;
    }
    output.push_back(static_cast<char>(std::tolower(byte)));
    previous_space = false;
  }
  if (!output.empty() && output.front() == ' ') {
    output.erase(output.begin());
  }
  if (!output.empty() && output.back() == ' ') {
    output.pop_back();
  }
  return output;
}

inline knowledge_eval_result evaluate_knowledge_retrieval(
  const knowledge_retriever& retriever,
  const std::vector<knowledge_eval_case>& cases) {
  knowledge_eval_result result;
  result.total = cases.size();
  result.cases.reserve(cases.size());

  double reciprocal_rank_sum = 0.0;
  for (const auto& item : cases) {
    knowledge_query query;
    query.text = item.query;
    query.filters = item.filters;
    query.limit = item.limit;

    knowledge_eval_case_result case_result {
      .name = item.name,
      .query = item.query,
    };

    const auto retrieved = retriever.retrieve(query);
    case_result.returned_document_ids.reserve(retrieved.size());
    std::string joined_content;
    for (std::size_t index = 0; index < retrieved.size(); ++index) {
      const auto& document_id = retrieved[index].chunk.document_id;
      case_result.returned_document_ids.push_back(document_id);
      joined_content += retrieved[index].chunk.content;
      joined_content.push_back('\n');
      if (!case_result.hit &&
          std::find(item.expected_document_ids.begin(),
            item.expected_document_ids.end(),
            document_id) != item.expected_document_ids.end()) {
        case_result.hit = true;
        case_result.reciprocal_rank = 1.0 / static_cast<double>(index + 1);
      }
    }
    const auto normalized_content = normalize_eval_text(joined_content);
    for (const auto& term : item.expected_terms) {
      if (!term.empty() &&
          normalized_content.find(normalize_eval_text(term)) == std::string::npos) {
        case_result.terms_hit = false;
        case_result.missing_terms.push_back(term);
      }
    }

    if (case_result.hit) {
      ++result.hits;
      reciprocal_rank_sum += case_result.reciprocal_rank;
    }
    if (case_result.terms_hit) {
      ++result.term_hits;
    }
    result.cases.push_back(std::move(case_result));
  }

  if (result.total != 0) {
    result.recall_at_k = static_cast<double>(result.hits) / static_cast<double>(result.total);
    result.term_recall =
      static_cast<double>(result.term_hits) / static_cast<double>(result.total);
    result.mean_reciprocal_rank =
      reciprocal_rank_sum / static_cast<double>(result.total);
  }
  return result;
}

inline nlohmann::json knowledge_eval_result_to_json(const knowledge_eval_result& result) {
  nlohmann::json data {
    { "total", result.total },
    { "hits", result.hits },
    { "term_hits", result.term_hits },
    { "recall_at_k", result.recall_at_k },
    { "term_recall", result.term_recall },
    { "mean_reciprocal_rank", result.mean_reciprocal_rank },
    { "cases", nlohmann::json::array() },
  };

  for (const auto& item : result.cases) {
    data["cases"].push_back({
      { "name", item.name },
      { "query", item.query },
      { "hit", item.hit },
      { "terms_hit", item.terms_hit },
      { "reciprocal_rank", item.reciprocal_rank },
      { "returned_document_ids", item.returned_document_ids },
      { "missing_terms", item.missing_terms },
    });
  }
  return data;
}

} // namespace wuwe::agent::knowledge

#endif // WUWE_AGENT_KNOWLEDGE_EVAL_HPP
