#ifndef WUWE_AGENT_KNOWLEDGE_EVAL_HPP
#define WUWE_AGENT_KNOWLEDGE_EVAL_HPP

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include <wuwe/agent/knowledge/knowledge_retriever.hpp>

namespace wuwe::agent::knowledge {

struct knowledge_eval_case {
  std::string name;
  std::string query;
  std::vector<std::string> expected_document_ids;
  std::map<std::string, std::string> filters;
  std::size_t limit { 6 };
};

struct knowledge_eval_case_result {
  std::string name;
  std::string query;
  bool hit {};
  double reciprocal_rank {};
  std::vector<std::string> returned_document_ids;
};

struct knowledge_eval_result {
  std::size_t total {};
  std::size_t hits {};
  double recall_at_k {};
  double mean_reciprocal_rank {};
  std::vector<knowledge_eval_case_result> cases;
};

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
    for (std::size_t index = 0; index < retrieved.size(); ++index) {
      const auto& document_id = retrieved[index].chunk.document_id;
      case_result.returned_document_ids.push_back(document_id);
      if (!case_result.hit &&
          std::find(item.expected_document_ids.begin(),
            item.expected_document_ids.end(),
            document_id) != item.expected_document_ids.end()) {
        case_result.hit = true;
        case_result.reciprocal_rank = 1.0 / static_cast<double>(index + 1);
      }
    }

    if (case_result.hit) {
      ++result.hits;
      reciprocal_rank_sum += case_result.reciprocal_rank;
    }
    result.cases.push_back(std::move(case_result));
  }

  if (result.total != 0) {
    result.recall_at_k = static_cast<double>(result.hits) / static_cast<double>(result.total);
    result.mean_reciprocal_rank =
      reciprocal_rank_sum / static_cast<double>(result.total);
  }
  return result;
}

} // namespace wuwe::agent::knowledge

#endif // WUWE_AGENT_KNOWLEDGE_EVAL_HPP
