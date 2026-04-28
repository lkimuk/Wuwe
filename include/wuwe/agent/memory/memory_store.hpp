#ifndef WUWE_AGENT_MEMORY_STORE_HPP
#define WUWE_AGENT_MEMORY_STORE_HPP

#include <optional>
#include <string>
#include <vector>

#include <wuwe/agent/memory/memory_record.hpp>

namespace wuwe::agent::memory {

class memory_store {
public:
  virtual ~memory_store() = default;

  virtual memory_record add(memory_record record) = 0;

  virtual std::optional<memory_record> get(
    const std::string& id, const memory_scope& scope) const = 0;

  virtual std::vector<memory_record> search(const memory_query& query) const = 0;

  virtual bool update(memory_record record) = 0;

  virtual bool erase(const std::string& id, const memory_scope& scope) = 0;

  virtual std::size_t clear(const memory_scope& scope) = 0;
};

} // namespace wuwe::agent::memory

#endif // WUWE_AGENT_MEMORY_STORE_HPP
