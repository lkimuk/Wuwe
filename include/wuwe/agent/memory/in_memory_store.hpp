#ifndef WUWE_AGENT_MEMORY_IN_MEMORY_STORE_HPP
#define WUWE_AGENT_MEMORY_IN_MEMORY_STORE_HPP

#include <algorithm>
#include <chrono>
#include <mutex>
#include <string>
#include <utility>

#include <wuwe/agent/core/text_search.hpp>
#include <wuwe/agent/memory/memory_store.hpp>

namespace wuwe::agent::memory {

namespace detail {

inline double lexical_score(const std::string& query, const memory_record& record) {
  if (query.empty()) {
    return record.score;
  }

  const auto content = record.content + " " + record.summary;

  double score = record.score;
  if (::wuwe::agent::text::contains_ascii_case_insensitive(content, query)) {
    score += 10.0;
  }

  score += ::wuwe::agent::text::ascii_token_set(query)
             .count_matches(::wuwe::agent::text::ascii_token_set(content));

  return score;
}

inline bool contains_kind(const std::vector<memory_kind>& kinds, memory_kind kind) {
  return kinds.empty() || std::find(kinds.begin(), kinds.end(), kind) != kinds.end();
}

inline bool metadata_matches(
  const std::map<std::string, std::string>& metadata,
  const std::map<std::string, std::string>& filters) {
  for (const auto& [key, value] : filters) {
    const auto it = metadata.find(key);
    if (it == metadata.end() || it->second != value) {
      return false;
    }
  }
  return true;
}

inline bool is_expired(
  const memory_record& record,
  std::chrono::system_clock::time_point now = std::chrono::system_clock::now()) {
  return record.expires_at && *record.expires_at <= now;
}

} // namespace detail

class in_memory_store final : public memory_store {
public:
  memory_record add(memory_record record) override {
    std::scoped_lock lock(mutex_);

    const auto now = std::chrono::system_clock::now();
    if (record.id.empty()) {
      record.id = "mem-" + std::to_string(++next_id_);
    }
    if (record.created_at == std::chrono::system_clock::time_point {}) {
      record.created_at = now;
    }
    record.updated_at = now;

    records_.push_back(record);
    return record;
  }

  std::optional<memory_record> get(
    const std::string& id, const memory_scope& scope) const override {
    std::scoped_lock lock(mutex_);

    const auto it = std::find_if(records_.begin(), records_.end(), [&](const memory_record& record) {
      return record.id == id && scope_matches(record.scope, scope);
    });

    if (it == records_.end()) {
      return std::nullopt;
    }

    return *it;
  }

  std::vector<memory_record> search(const memory_query& query) const override {
    std::scoped_lock lock(mutex_);

    std::vector<memory_record> result;

    for (auto record : records_) {
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

      record.score = detail::lexical_score(query.text, record);
      result.push_back(std::move(record));
    }

    std::sort(result.begin(), result.end(), [](const memory_record& lhs, const memory_record& rhs) {
      if (lhs.score != rhs.score) {
        return lhs.score > rhs.score;
      }
      if (lhs.priority != rhs.priority) {
        return lhs.priority > rhs.priority;
      }
      return lhs.updated_at > rhs.updated_at;
    });

    if (result.size() > query.limit) {
      result.resize(query.limit);
    }

    return result;
  }

  bool update(memory_record record) override {
    std::scoped_lock lock(mutex_);

    const auto it = std::find_if(records_.begin(), records_.end(), [&](const memory_record& current) {
      return current.id == record.id && scope_matches(current.scope, record.scope);
    });

    if (it == records_.end()) {
      return false;
    }

    record.created_at = it->created_at;
    record.updated_at = std::chrono::system_clock::now();
    *it = std::move(record);
    return true;
  }

  bool erase(const std::string& id, const memory_scope& scope) override {
    std::scoped_lock lock(mutex_);

    const auto old_size = records_.size();
    std::erase_if(records_, [&](const memory_record& record) {
      return record.id == id && scope_matches(record.scope, scope);
    });
    return records_.size() != old_size;
  }

  std::size_t clear(const memory_scope& scope) override {
    std::scoped_lock lock(mutex_);

    const auto old_size = records_.size();
    std::erase_if(records_, [&](const memory_record& record) {
      return scope_matches(record.scope, scope);
    });
    return old_size - records_.size();
  }

private:
  mutable std::mutex mutex_;
  std::vector<memory_record> records_;
  std::size_t next_id_ { 0 };
};

} // namespace wuwe::agent::memory

#endif // WUWE_AGENT_MEMORY_IN_MEMORY_STORE_HPP
