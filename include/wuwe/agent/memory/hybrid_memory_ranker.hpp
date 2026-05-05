#ifndef WUWE_AGENT_MEMORY_HYBRID_MEMORY_RANKER_HPP
#define WUWE_AGENT_MEMORY_HYBRID_MEMORY_RANKER_HPP

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>
#include <vector>

#include <wuwe/agent/memory/in_memory_store.hpp>
#include <wuwe/agent/memory/memory_ranker.hpp>

namespace wuwe::agent::memory {

struct hybrid_memory_ranker_policy {
  double vector_weight { 0.65 };
  double lexical_weight { 0.20 };
  double priority_weight { 0.10 };
  double recency_weight { 0.05 };
  double minimum_vector_score { -1.0 };
  int priority_scale { 10 };
  std::chrono::seconds recency_half_life { std::chrono::hours(24 * 30) };
};

class hybrid_memory_ranker final : public memory_ranker {
public:
  explicit hybrid_memory_ranker(hybrid_memory_ranker_policy policy = {})
      : policy_(std::move(policy)) {
  }

  std::vector<memory_record> rank(
    const memory_query& query,
    std::vector<memory_record> candidates) const override {
    const auto now = std::chrono::system_clock::now();

    std::erase_if(candidates, [&](const memory_record& record) {
      return vector_score(record) < policy_.minimum_vector_score;
    });

    std::sort(candidates.begin(), candidates.end(), [&](const auto& lhs, const auto& rhs) {
      const auto lhs_score = final_score(query, lhs, now);
      const auto rhs_score = final_score(query, rhs, now);
      if (lhs_score != rhs_score) {
        return lhs_score > rhs_score;
      }
      if (lhs.priority != rhs.priority) {
        return lhs.priority > rhs.priority;
      }
      return lhs.updated_at > rhs.updated_at;
    });

    if (candidates.size() > query.limit) {
      candidates.resize(query.limit);
    }
    return candidates;
  }

  const hybrid_memory_ranker_policy& policy() const noexcept {
    return policy_;
  }

private:
  double final_score(
    const memory_query& query,
    const memory_record& record,
    std::chrono::system_clock::time_point now) const {
    return policy_.vector_weight * vector_score(record) +
           policy_.lexical_weight * lexical_score(query, record) +
           policy_.priority_weight * priority_score(record) +
           policy_.recency_weight * recency_score(record, now);
  }

  static double vector_score(const memory_record& record) {
    const auto it = record.metadata.find("vector_score");
    if (it == record.metadata.end()) {
      return 0.0;
    }
    try {
      return std::stod(it->second);
    }
    catch (...) {
      return 0.0;
    }
  }

  static double lexical_score(const memory_query& query, const memory_record& record) {
    const double raw = detail::lexical_score(query.text, record);
    return raw <= 0.0 ? 0.0 : raw / (raw + 10.0);
  }

  double priority_score(const memory_record& record) const {
    if (policy_.priority_scale <= 0) {
      return record.priority > 0 ? 1.0 : 0.0;
    }
    const double scaled =
      static_cast<double>(record.priority) / static_cast<double>(policy_.priority_scale);
    return (std::clamp)(scaled, 0.0, 1.0);
  }

  double recency_score(
    const memory_record& record,
    std::chrono::system_clock::time_point now) const {
    if (record.updated_at == std::chrono::system_clock::time_point {}) {
      return 0.0;
    }
    if (policy_.recency_half_life.count() <= 0) {
      return 0.0;
    }

    const auto age = now > record.updated_at ? now - record.updated_at : std::chrono::seconds(0);
    const auto age_seconds =
      std::chrono::duration_cast<std::chrono::duration<double>>(age).count();
    const auto half_life_seconds =
      std::chrono::duration_cast<std::chrono::duration<double>>(policy_.recency_half_life).count();
    return std::pow(0.5, age_seconds / half_life_seconds);
  }

private:
  hybrid_memory_ranker_policy policy_;
};

} // namespace wuwe::agent::memory

#endif // WUWE_AGENT_MEMORY_HYBRID_MEMORY_RANKER_HPP
