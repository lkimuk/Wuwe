#ifndef WUWE_AGENT_MEMORY_IN_MEMORY_VECTOR_INDEX_HPP
#define WUWE_AGENT_MEMORY_IN_MEMORY_VECTOR_INDEX_HPP

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

#include <wuwe/agent/memory/in_memory_store.hpp>
#include <wuwe/agent/memory/vector_memory_index.hpp>

namespace wuwe::agent::memory {

class in_memory_vector_index final : public vector_memory_index {
public:
  void upsert(const memory_record& record, const std::vector<float>& embedding) override {
    std::scoped_lock lock(mutex_);

    const auto it = std::find_if(points_.begin(), points_.end(), [&](const point& item) {
      return item.record.id == record.id && scope_matches(item.record.scope, record.scope);
    });

    point updated {
      .record = record,
      .embedding = embedding,
    };

    if (it == points_.end()) {
      points_.push_back(std::move(updated));
      return;
    }

    *it = std::move(updated);
  }

  void upsert_batch(
    const std::vector<memory_record>& records,
    const std::vector<std::vector<float>>& embeddings) override {
    if (records.size() != embeddings.size()) {
      throw std::invalid_argument("in_memory_vector_index upsert_batch size mismatch");
    }
    for (std::size_t index = 0; index < records.size(); ++index) {
      upsert(records[index], embeddings[index]);
    }
  }

  std::vector<vector_memory_match> search(const vector_memory_query& query) const override {
    std::scoped_lock lock(mutex_);

    std::vector<vector_memory_match> matches;
    for (const auto& point : points_) {
      const auto& record = point.record;
      if (!scope_matches(record.scope, query.scope)) {
        continue;
      }
      if (!detail::contains_kind(query.kinds, record.kind)) {
        continue;
      }
      if (!query.include_expired && detail::is_expired(record)) {
        continue;
      }
      if (!detail::metadata_matches(record.metadata, query.filters)) {
        continue;
      }

      matches.push_back({
        .memory_id = record.id,
        .score = vector_detail::cosine_similarity(query.embedding, point.embedding),
      });
    }

    std::sort(matches.begin(), matches.end(), [](const auto& lhs, const auto& rhs) {
      return lhs.score > rhs.score;
    });

    if (matches.size() > query.limit) {
      matches.resize(query.limit);
    }
    return matches;
  }

  bool erase(const std::string& memory_id, const memory_scope& scope) override {
    std::scoped_lock lock(mutex_);

    const auto old_size = points_.size();
    std::erase_if(points_, [&](const point& item) {
      return item.record.id == memory_id && scope_matches(item.record.scope, scope);
    });
    return points_.size() != old_size;
  }

  std::size_t clear(const memory_scope& scope) override {
    std::scoped_lock lock(mutex_);

    const auto old_size = points_.size();
    std::erase_if(points_, [&](const point& item) {
      return scope_matches(item.record.scope, scope);
    });
    return old_size - points_.size();
  }

private:
  struct point {
    memory_record record;
    std::vector<float> embedding;
  };

  mutable std::mutex mutex_;
  std::vector<point> points_;
};

} // namespace wuwe::agent::memory

#endif // WUWE_AGENT_MEMORY_IN_MEMORY_VECTOR_INDEX_HPP
