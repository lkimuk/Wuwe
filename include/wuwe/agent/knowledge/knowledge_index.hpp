#ifndef WUWE_AGENT_KNOWLEDGE_INDEX_HPP
#define WUWE_AGENT_KNOWLEDGE_INDEX_HPP

#include <stdexcept>
#include <string>
#include <vector>

#include <wuwe/agent/knowledge/knowledge_record.hpp>

namespace wuwe::agent::knowledge {

class knowledge_index {
public:
  virtual ~knowledge_index() = default;

  virtual void upsert(const knowledge_chunk& chunk, const std::vector<float>& embedding) = 0;

  virtual void upsert_batch(
    const std::vector<knowledge_chunk>& chunks,
    const std::vector<std::vector<float>>& embeddings) {
    if (chunks.size() != embeddings.size()) {
      throw std::invalid_argument("knowledge_index upsert_batch size mismatch");
    }

    for (std::size_t index = 0; index < chunks.size(); ++index) {
      upsert(chunks[index], embeddings[index]);
    }
  }

  virtual std::vector<knowledge_result> search(
    const knowledge_query& query,
    const std::vector<float>& embedding) const = 0;

  virtual bool erase_document(const std::string& document_id) = 0;

  virtual void clear() = 0;
};

} // namespace wuwe::agent::knowledge

#endif // WUWE_AGENT_KNOWLEDGE_INDEX_HPP
