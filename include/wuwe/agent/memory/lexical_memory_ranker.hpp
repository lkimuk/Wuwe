#ifndef WUWE_AGENT_MEMORY_LEXICAL_MEMORY_RANKER_HPP
#define WUWE_AGENT_MEMORY_LEXICAL_MEMORY_RANKER_HPP

#include <algorithm>
#include <utility>
#include <vector>

#include <wuwe/agent/memory/in_memory_store.hpp>
#include <wuwe/agent/memory/memory_ranker.hpp>

namespace wuwe::agent::memory {

class lexical_memory_ranker final : public memory_ranker {
public:
  std::vector<memory_record> rank(
    const memory_query& query,
    std::vector<memory_record> candidates) const override {
    for (auto& candidate : candidates) {
      candidate.score = detail::lexical_score(query.text, candidate);
    }

    std::sort(candidates.begin(), candidates.end(), [](const memory_record& lhs,
                                                        const memory_record& rhs) {
      if (lhs.score != rhs.score) {
        return lhs.score > rhs.score;
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
};

} // namespace wuwe::agent::memory

#endif // WUWE_AGENT_MEMORY_LEXICAL_MEMORY_RANKER_HPP
