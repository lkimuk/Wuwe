#ifndef WUWE_AGENT_KNOWLEDGE_STORE_HPP
#define WUWE_AGENT_KNOWLEDGE_STORE_HPP

#include <string>
#include <vector>

#include <wuwe/agent/knowledge/knowledge_record.hpp>

namespace wuwe::agent::knowledge {

class knowledge_store {
public:
  virtual ~knowledge_store() = default;

  virtual void add_document(knowledge_document document) = 0;

  virtual std::vector<knowledge_document> list_documents() const = 0;

  virtual void add_chunks(std::vector<knowledge_chunk> chunks) = 0;

  virtual std::vector<knowledge_chunk> list_chunks(const knowledge_query& query) const = 0;

  virtual bool erase_document(const std::string& document_id) = 0;

  virtual void clear() = 0;
};

} // namespace wuwe::agent::knowledge

#endif // WUWE_AGENT_KNOWLEDGE_STORE_HPP
