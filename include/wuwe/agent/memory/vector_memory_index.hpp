#ifndef WUWE_AGENT_MEMORY_VECTOR_MEMORY_INDEX_HPP
#define WUWE_AGENT_MEMORY_VECTOR_MEMORY_INDEX_HPP

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <stdexcept>
#include <vector>

#include <wuwe/agent/memory/memory_record.hpp>

namespace wuwe::agent::memory {

struct vector_memory_query {
  std::vector<float> embedding;
  memory_scope scope;
  std::vector<memory_kind> kinds;
  std::size_t limit { 8 };
  std::map<std::string, std::string> filters;
  bool include_expired { false };
};

struct vector_memory_match {
  std::string memory_id;
  double score { 0.0 };
};

class vector_memory_index {
public:
  virtual ~vector_memory_index() = default;

  virtual void upsert(const memory_record& record, const std::vector<float>& embedding) = 0;

  virtual void upsert_batch(
    const std::vector<memory_record>& records,
    const std::vector<std::vector<float>>& embeddings) {
    if (records.size() != embeddings.size()) {
      throw std::invalid_argument("vector_memory_index upsert_batch size mismatch");
    }
    for (std::size_t index = 0; index < records.size(); ++index) {
      upsert(records[index], embeddings[index]);
    }
  }

  virtual std::vector<vector_memory_match> search(const vector_memory_query& query) const = 0;

  virtual bool erase(const std::string& memory_id, const memory_scope& scope) = 0;

  virtual std::size_t clear(const memory_scope& scope) = 0;
};

namespace vector_detail {

inline double dot_product(const std::vector<float>& lhs, const std::vector<float>& rhs) {
  double result = 0.0;
  const auto size = (std::min)(lhs.size(), rhs.size());
  for (std::size_t index = 0; index < size; ++index) {
    result += static_cast<double>(lhs[index]) * static_cast<double>(rhs[index]);
  }
  return result;
}

inline double cosine_similarity(const std::vector<float>& lhs, const std::vector<float>& rhs) {
  if (lhs.empty() || rhs.empty() || lhs.size() != rhs.size()) {
    return 0.0;
  }

  const double lhs_norm = dot_product(lhs, lhs);
  const double rhs_norm = dot_product(rhs, rhs);
  if (lhs_norm <= 0.0 || rhs_norm <= 0.0) {
    return 0.0;
  }

  return dot_product(lhs, rhs) / std::sqrt(lhs_norm * rhs_norm);
}

inline std::int64_t to_unix_millis(std::chrono::system_clock::time_point value) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(value.time_since_epoch()).count();
}

} // namespace vector_detail

} // namespace wuwe::agent::memory

#endif // WUWE_AGENT_MEMORY_VECTOR_MEMORY_INDEX_HPP
