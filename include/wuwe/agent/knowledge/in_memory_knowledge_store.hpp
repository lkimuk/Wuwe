#ifndef WUWE_AGENT_KNOWLEDGE_IN_MEMORY_KNOWLEDGE_STORE_HPP
#define WUWE_AGENT_KNOWLEDGE_IN_MEMORY_KNOWLEDGE_STORE_HPP

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <wuwe/agent/knowledge/knowledge_store.hpp>

namespace wuwe::agent::knowledge {

class in_memory_knowledge_store final : public knowledge_store {
public:
  void add_document(knowledge_document document) override {
    std::scoped_lock lock(mutex_);
    documents_[document.id] = std::move(document);
  }

  std::vector<knowledge_document> list_documents() const override {
    std::scoped_lock lock(mutex_);

    std::vector<knowledge_document> result;
    result.reserve(documents_.size());
    for (const auto& [_, document] : documents_) {
      result.push_back(document);
    }
    std::sort(result.begin(), result.end(), [](const knowledge_document& lhs,
                                                const knowledge_document& rhs) {
      return lhs.id < rhs.id;
    });
    return result;
  }

  void add_chunks(std::vector<knowledge_chunk> chunks) override {
    std::scoped_lock lock(mutex_);
    for (auto& chunk : chunks) {
      chunks_[chunk.id] = std::move(chunk);
    }
  }

  std::vector<knowledge_chunk> list_chunks(const knowledge_query& query) const override {
    std::scoped_lock lock(mutex_);

    std::vector<knowledge_chunk> result;
    for (const auto& [_, chunk] : chunks_) {
      if (!metadata_matches(chunk.metadata, query.filters) ||
          !metadata_access_matches(chunk.metadata, query.access)) {
        continue;
      }
      result.push_back(chunk);
    }

    std::sort(result.begin(), result.end(), [](const knowledge_chunk& lhs,
                                                const knowledge_chunk& rhs) {
      if (lhs.document_id != rhs.document_id) {
        return lhs.document_id < rhs.document_id;
      }
      return lhs.start_offset < rhs.start_offset;
    });

    if (query.limit != 0 && result.size() > query.limit) {
      result.resize(query.limit);
    }
    return result;
  }

  bool erase_document(const std::string& document_id) override {
    std::scoped_lock lock(mutex_);
    const bool erased_document = documents_.erase(document_id) != 0;
    const auto old_size = chunks_.size();

    for (auto it = chunks_.begin(); it != chunks_.end();) {
      if (it->second.document_id == document_id) {
        it = chunks_.erase(it);
      }
      else {
        ++it;
      }
    }

    return erased_document || chunks_.size() != old_size;
  }

  void clear() override {
    std::scoped_lock lock(mutex_);
    documents_.clear();
    chunks_.clear();
  }

private:
  mutable std::mutex mutex_;
  std::unordered_map<std::string, knowledge_document> documents_;
  std::unordered_map<std::string, knowledge_chunk> chunks_;
};

} // namespace wuwe::agent::knowledge

#endif // WUWE_AGENT_KNOWLEDGE_IN_MEMORY_KNOWLEDGE_STORE_HPP
