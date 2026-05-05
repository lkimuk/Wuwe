#ifndef WUWE_AGENT_KNOWLEDGE_RESULT_PROCESSOR_HPP
#define WUWE_AGENT_KNOWLEDGE_RESULT_PROCESSOR_HPP

#include <algorithm>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <wuwe/agent/knowledge/knowledge_record.hpp>

namespace wuwe::agent::knowledge {

struct knowledge_result_processing_policy {
  bool dedupe_chunks { true };
  bool merge_adjacent_chunks { true };
  std::size_t max_merged_chars { 2400 };
  std::size_t max_line_gap_for_merge { 1 };
};

class knowledge_result_processor {
public:
  explicit knowledge_result_processor(knowledge_result_processing_policy policy = {})
      : policy_(policy) {
  }

  std::vector<knowledge_result> process(std::vector<knowledge_result> results) const {
    if (policy_.dedupe_chunks) {
      results = dedupe(std::move(results));
    }
    if (policy_.merge_adjacent_chunks) {
      results = merge_adjacent(std::move(results));
    }
    return results;
  }

private:
  std::vector<knowledge_result> dedupe(std::vector<knowledge_result> results) const {
    std::vector<knowledge_result> deduped;
    std::set<std::string> seen_ids;
    std::set<std::string> seen_content;

    for (auto& result : results) {
      const auto content_key = result.chunk.document_id + "\n" + result.chunk.content;
      if ((!result.chunk.id.empty() && !seen_ids.insert(result.chunk.id).second) ||
          !seen_content.insert(content_key).second) {
        continue;
      }
      deduped.push_back(std::move(result));
    }
    return deduped;
  }

  std::vector<knowledge_result> merge_adjacent(std::vector<knowledge_result> results) const {
    if (results.size() < 2) {
      return results;
    }

    std::vector<knowledge_result> ordered = results;
    std::sort(ordered.begin(), ordered.end(), [](const knowledge_result& lhs,
                                                  const knowledge_result& rhs) {
      if (lhs.chunk.document_id != rhs.chunk.document_id) {
        return lhs.chunk.document_id < rhs.chunk.document_id;
      }
      return lhs.chunk.start_offset < rhs.chunk.start_offset;
    });

    std::vector<knowledge_result> merged;
    for (auto& result : ordered) {
      if (merged.empty() || !can_merge(merged.back(), result)) {
        merged.push_back(std::move(result));
        continue;
      }
      merge_into(merged.back(), result);
    }

    std::sort(merged.begin(), merged.end(), [](const knowledge_result& lhs,
                                                const knowledge_result& rhs) {
      if (lhs.score != rhs.score) {
        return lhs.score > rhs.score;
      }
      if (lhs.chunk.document_id != rhs.chunk.document_id) {
        return lhs.chunk.document_id < rhs.chunk.document_id;
      }
      return lhs.chunk.start_offset < rhs.chunk.start_offset;
    });
    return merged;
  }

  bool can_merge(const knowledge_result& lhs, const knowledge_result& rhs) const {
    if (lhs.chunk.document_id != rhs.chunk.document_id ||
        lhs.chunk.source_uri != rhs.chunk.source_uri) {
      return false;
    }
    if (lhs.chunk.end_offset > rhs.chunk.start_offset) {
      return false;
    }
    if (lhs.chunk.content.size() + rhs.chunk.content.size() > policy_.max_merged_chars) {
      return false;
    }
    if (lhs.chunk.end_line != 0 && rhs.chunk.start_line != 0 &&
        rhs.chunk.start_line > lhs.chunk.end_line + policy_.max_line_gap_for_merge) {
      return false;
    }
    return true;
  }

  static void merge_into(knowledge_result& lhs, const knowledge_result& rhs) {
    lhs.chunk.id += "+" + rhs.chunk.id;
    lhs.chunk.content += "\n";
    lhs.chunk.content += rhs.chunk.content;
    lhs.chunk.end_offset = rhs.chunk.end_offset;
    lhs.chunk.end_line = rhs.chunk.end_line;
    lhs.score = (std::max)(lhs.score, rhs.score);
    lhs.vector_score = (std::max)(lhs.vector_score, rhs.vector_score);
    lhs.lexical_score = (std::max)(lhs.lexical_score, rhs.lexical_score);
  }

  knowledge_result_processing_policy policy_;
};

} // namespace wuwe::agent::knowledge

#endif // WUWE_AGENT_KNOWLEDGE_RESULT_PROCESSOR_HPP
