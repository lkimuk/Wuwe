#ifndef WUWE_AGENT_MEMORY_RANKER_HPP
#define WUWE_AGENT_MEMORY_RANKER_HPP

#include <vector>

#include <wuwe/agent/memory/memory_record.hpp>

namespace wuwe::agent::memory {

class memory_ranker {
public:
  virtual ~memory_ranker() = default;

  virtual std::vector<memory_record> rank(
    const memory_query& query,
    std::vector<memory_record> candidates) const = 0;
};

} // namespace wuwe::agent::memory

#endif // WUWE_AGENT_MEMORY_RANKER_HPP
