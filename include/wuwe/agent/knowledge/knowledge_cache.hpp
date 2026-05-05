#ifndef WUWE_AGENT_KNOWLEDGE_CACHE_HPP
#define WUWE_AGENT_KNOWLEDGE_CACHE_HPP

#include <algorithm>
#include <chrono>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <wuwe/agent/knowledge/knowledge_record.hpp>
#include <wuwe/agent/knowledge/knowledge_reranker.hpp>

namespace wuwe::agent::knowledge {

inline std::string knowledge_query_cache_key(const knowledge_query& query) {
  std::ostringstream output;
  output << query.text << "|limit=" << query.limit
         << "|candidate_limit=" << query.candidate_limit
         << "|min=" << query.minimum_score
         << "|vw=" << query.vector_weight
         << "|lw=" << query.lexical_weight
         << "|tenant=" << query.access.tenant_id
         << "|user=" << query.access.user_id
         << "|bypass=" << query.access.bypass_acl;
  for (const auto& role : query.access.roles) {
    output << "|role=" << role;
  }
  for (const auto& [key, value] : query.filters) {
    output << "|filter=" << key << '=' << value;
  }
  return output.str();
}

class knowledge_retrieval_cache {
public:
  virtual ~knowledge_retrieval_cache() = default;

  virtual bool get(const std::string& key, std::vector<knowledge_result>& results) const = 0;
  virtual void put(std::string key, std::vector<knowledge_result> results) = 0;
  virtual void clear() = 0;
};

class in_memory_knowledge_retrieval_cache final : public knowledge_retrieval_cache {
public:
  explicit in_memory_knowledge_retrieval_cache(
    std::size_t max_entries = 1024,
    std::chrono::milliseconds ttl = std::chrono::milliseconds::zero())
      : max_entries_(max_entries), ttl_(ttl) {
  }

  bool get(const std::string& key, std::vector<knowledge_result>& results) const override {
    std::scoped_lock lock(mutex_);
    const auto it = entries_.find(key);
    if (it == entries_.end()) {
      return false;
    }
    if (expired(it->second)) {
      order_.erase(it->second.order);
      entries_.erase(it);
      return false;
    }
    order_.splice(order_.begin(), order_, it->second.order);
    results = it->second.results;
    return true;
  }

  void put(std::string key, std::vector<knowledge_result> results) override {
    std::scoped_lock lock(mutex_);
    prune_expired();
    const auto existing = entries_.find(key);
    if (existing != entries_.end()) {
      existing->second.results = std::move(results);
      existing->second.created_at = clock::now();
      order_.splice(order_.begin(), order_, existing->second.order);
      return;
    }

    if (max_entries_ == 0) {
      return;
    }
    while (entries_.size() >= max_entries_ && !order_.empty()) {
      entries_.erase(order_.back());
      order_.pop_back();
    }
    order_.push_front(key);
    entries_[std::move(key)] = entry {
      .results = std::move(results),
      .created_at = clock::now(),
      .order = order_.begin(),
    };
  }

  void clear() override {
    std::scoped_lock lock(mutex_);
    entries_.clear();
    order_.clear();
  }

private:
  using clock = std::chrono::steady_clock;

  struct entry {
    std::vector<knowledge_result> results;
    clock::time_point created_at;
    std::list<std::string>::iterator order;
  };

  bool expired(const entry& value) const {
    return ttl_ != std::chrono::milliseconds::zero() &&
           clock::now() - value.created_at >= ttl_;
  }

  void prune_expired() const {
    if (ttl_ == std::chrono::milliseconds::zero()) {
      return;
    }
    for (auto it = entries_.begin(); it != entries_.end();) {
      if (expired(it->second)) {
        order_.erase(it->second.order);
        it = entries_.erase(it);
      }
      else {
        ++it;
      }
    }
  }

  std::size_t max_entries_;
  std::chrono::milliseconds ttl_;
  mutable std::mutex mutex_;
  mutable std::list<std::string> order_;
  mutable std::unordered_map<std::string, entry> entries_;
};

class cached_knowledge_reranker final : public knowledge_reranker {
public:
  cached_knowledge_reranker(
    std::shared_ptr<knowledge_reranker> inner,
    std::size_t max_entries = 1024,
    std::chrono::milliseconds ttl = std::chrono::milliseconds::zero())
      : inner_(std::move(inner)), max_entries_(max_entries), ttl_(ttl) {
    if (!inner_) {
      throw std::invalid_argument("cached_knowledge_reranker requires inner reranker");
    }
  }

  std::vector<knowledge_result> rerank(
    const knowledge_query& query,
    std::vector<knowledge_result> candidates) const override {
    const auto key = rerank_key(query, candidates);
    {
      std::scoped_lock lock(mutex_);
      const auto it = entries_.find(key);
      if (it != entries_.end()) {
        if (expired(it->second)) {
          order_.erase(it->second.order);
          entries_.erase(it);
        }
        else {
          order_.splice(order_.begin(), order_, it->second.order);
          return it->second.results;
        }
      }
    }

    auto results = inner_->rerank(query, std::move(candidates));
    {
      std::scoped_lock lock(mutex_);
      prune_expired();
      if (max_entries_ != 0) {
        while (entries_.size() >= max_entries_ && !order_.empty()) {
          entries_.erase(order_.back());
          order_.pop_back();
        }
        order_.push_front(key);
        entries_[key] = entry {
          .results = results,
          .created_at = clock::now(),
          .order = order_.begin(),
        };
      }
    }
    return results;
  }

  void clear() {
    std::scoped_lock lock(mutex_);
    entries_.clear();
    order_.clear();
  }

private:
  using clock = std::chrono::steady_clock;

  struct entry {
    std::vector<knowledge_result> results;
    clock::time_point created_at;
    std::list<std::string>::iterator order;
  };

  static std::string rerank_key(
    const knowledge_query& query,
    const std::vector<knowledge_result>& candidates) {
    std::ostringstream output;
    output << knowledge_query_cache_key(query);
    for (const auto& candidate : candidates) {
      output << "|candidate="
             << (candidate.chunk.id.empty() ? candidate.chunk.document_id : candidate.chunk.id)
             << ':' << candidate.score;
    }
    return output.str();
  }

  bool expired(const entry& value) const {
    return ttl_ != std::chrono::milliseconds::zero() &&
           clock::now() - value.created_at >= ttl_;
  }

  void prune_expired() const {
    if (ttl_ == std::chrono::milliseconds::zero()) {
      return;
    }
    for (auto it = entries_.begin(); it != entries_.end();) {
      if (expired(it->second)) {
        order_.erase(it->second.order);
        it = entries_.erase(it);
      }
      else {
        ++it;
      }
    }
  }

  std::shared_ptr<knowledge_reranker> inner_;
  std::size_t max_entries_;
  std::chrono::milliseconds ttl_;
  mutable std::mutex mutex_;
  mutable std::list<std::string> order_;
  mutable std::unordered_map<std::string, entry> entries_;
};

} // namespace wuwe::agent::knowledge

#endif // WUWE_AGENT_KNOWLEDGE_CACHE_HPP
