#ifndef WUWE_AGENT_KNOWLEDGE_IN_MEMORY_KNOWLEDGE_INDEX_HPP
#define WUWE_AGENT_KNOWLEDGE_IN_MEMORY_KNOWLEDGE_INDEX_HPP

#include <algorithm>
#include <cctype>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <utility>
#include <vector>

#include <wuwe/agent/knowledge/knowledge_index.hpp>
#include <wuwe/agent/memory/vector_memory_index.hpp>

namespace wuwe::agent::knowledge {

namespace detail {

inline std::unordered_set<std::string> tokenize_knowledge_text(const std::string& text) {
  std::unordered_set<std::string> result;
  std::string current;
  for (const unsigned char ch : text) {
    if (std::isalnum(ch)) {
      current.push_back(static_cast<char>(std::tolower(ch)));
    }
    else if (!current.empty()) {
      result.insert(std::move(current));
      current.clear();
    }
  }
  if (!current.empty()) {
    result.insert(std::move(current));
  }
  return result;
}

inline double lexical_knowledge_score(const std::string& query, const knowledge_chunk& chunk) {
  if (query.empty()) {
    return 0.0;
  }

  const auto query_tokens = tokenize_knowledge_text(query);
  if (query_tokens.empty()) {
    return 0.0;
  }

  const auto content_tokens = tokenize_knowledge_text(
    chunk.title + " " + chunk.content + " " + chunk.source_uri);

  std::size_t matches = 0;
  for (const auto& token : query_tokens) {
    if (content_tokens.contains(token)) {
      ++matches;
    }
  }

  return static_cast<double>(matches) / static_cast<double>(query_tokens.size());
}

} // namespace detail

class in_memory_knowledge_index final : public knowledge_index {
public:
  void upsert(const knowledge_chunk& chunk, const std::vector<float>& embedding) override {
    std::scoped_lock lock(mutex_);
    entries_[chunk.id] = entry {
      .chunk = chunk,
      .embedding = embedding,
    };
  }

  std::vector<knowledge_result> search(
    const knowledge_query& query,
    const std::vector<float>& embedding) const override {
    std::scoped_lock lock(mutex_);

    std::vector<knowledge_result> result;
    for (const auto& [_, item] : entries_) {
      if (!metadata_matches(item.chunk.metadata, query.filters) ||
          !metadata_access_matches(item.chunk.metadata, query.access)) {
        continue;
      }

      const auto vector_score =
        ::wuwe::agent::memory::vector_detail::cosine_similarity(embedding, item.embedding);
      const auto lexical_score = detail::lexical_knowledge_score(query.text, item.chunk);
      const auto score =
        query.vector_weight * vector_score + query.lexical_weight * lexical_score;
      if (score < query.minimum_score) {
        continue;
      }

      result.push_back({
        .chunk = item.chunk,
        .score = score,
        .vector_score = vector_score,
        .lexical_score = lexical_score,
      });
    }

    std::sort(result.begin(), result.end(), [](const knowledge_result& lhs,
                                                const knowledge_result& rhs) {
      if (lhs.score != rhs.score) {
        return lhs.score > rhs.score;
      }
      if (lhs.chunk.document_id != rhs.chunk.document_id) {
        return lhs.chunk.document_id < rhs.chunk.document_id;
      }
      return lhs.chunk.start_offset < rhs.chunk.start_offset;
    });

    if (query.limit != 0 && result.size() > query.limit) {
      result.resize(query.limit);
    }
    return result;
  }

  bool erase_document(const std::string& document_id) override {
    std::scoped_lock lock(mutex_);
    const auto old_size = entries_.size();

    for (auto it = entries_.begin(); it != entries_.end();) {
      if (it->second.chunk.document_id == document_id) {
        it = entries_.erase(it);
      }
      else {
        ++it;
      }
    }

    return old_size != entries_.size();
  }

  void clear() override {
    std::scoped_lock lock(mutex_);
    entries_.clear();
  }

private:
  struct entry {
    knowledge_chunk chunk;
    std::vector<float> embedding;
  };

  mutable std::mutex mutex_;
  std::unordered_map<std::string, entry> entries_;
};

} // namespace wuwe::agent::knowledge

#endif // WUWE_AGENT_KNOWLEDGE_IN_MEMORY_KNOWLEDGE_INDEX_HPP
