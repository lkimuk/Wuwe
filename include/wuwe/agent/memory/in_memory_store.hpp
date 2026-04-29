#ifndef WUWE_AGENT_MEMORY_IN_MEMORY_STORE_HPP
#define WUWE_AGENT_MEMORY_IN_MEMORY_STORE_HPP

#include <algorithm>
#include <chrono>
#include <cctype>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>

#include <wuwe/agent/memory/memory_store.hpp>

namespace wuwe::agent::memory {

namespace detail {

inline std::string lower_copy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

inline std::unordered_set<std::string> tokenize(const std::string& text) {
  std::unordered_set<std::string> result;
  std::string current;

  for (const unsigned char c : text) {
    if (std::isalnum(c)) {
      current.push_back(static_cast<char>(std::tolower(c)));
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

inline double lexical_score(const std::string& query, const memory_record& record) {
  if (query.empty()) {
    return record.score;
  }

  const std::string query_lower = lower_copy(query);
  const std::string content_lower = lower_copy(record.content + " " + record.summary);

  double score = record.score;
  if (content_lower.find(query_lower) != std::string::npos) {
    score += 10.0;
  }

  const auto query_tokens = tokenize(query);
  const auto content_tokens = tokenize(record.content + " " + record.summary);
  for (const auto& token : query_tokens) {
    if (content_tokens.contains(token)) {
      score += 1.0;
    }
  }

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
