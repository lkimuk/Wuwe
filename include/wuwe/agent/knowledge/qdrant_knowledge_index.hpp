#ifndef WUWE_AGENT_KNOWLEDGE_QDRANT_KNOWLEDGE_INDEX_HPP
#define WUWE_AGENT_KNOWLEDGE_QDRANT_KNOWLEDGE_INDEX_HPP

#include <memory>
#include <string>

#include <wuwe/agent/knowledge/knowledge_index.hpp>
#include <wuwe/net/http_client.h>

namespace wuwe::agent::knowledge {

struct qdrant_knowledge_index_config {
  std::string base_url { "http://localhost:6333" };
  std::string api_key;
  std::string collection_name { "wuwe_knowledge" };
  std::string vector_name;
  std::string distance { "Cosine" };
  std::string embedding_provider;
  std::string embedding_model;
  std::string embedding_version;
  std::string index_schema_version { "1" };
  int timeout { 30000 };
  bool create_collection_if_missing { true };
};

class qdrant_knowledge_index final : public knowledge_index {
public:
  explicit qdrant_knowledge_index(qdrant_knowledge_index_config config);
  qdrant_knowledge_index(
    qdrant_knowledge_index_config config,
    std::shared_ptr<::wuwe::http_client> http);

  void upsert(const knowledge_chunk& chunk, const std::vector<float>& embedding) override;
  void upsert_batch(
    const std::vector<knowledge_chunk>& chunks,
    const std::vector<std::vector<float>>& embeddings) override;

  std::vector<knowledge_result> search(
    const knowledge_query& query,
    const std::vector<float>& embedding) const override;

  bool erase_document(const std::string& document_id) override;

  void clear() override;

private:
  void ensure_collection(std::size_t vector_size) const;
  std::string endpoint(std::string path) const;

  qdrant_knowledge_index_config config_;
  std::shared_ptr<::wuwe::http_client> http_;
  mutable bool collection_checked_ { false };
};

} // namespace wuwe::agent::knowledge

#endif // WUWE_AGENT_KNOWLEDGE_QDRANT_KNOWLEDGE_INDEX_HPP
