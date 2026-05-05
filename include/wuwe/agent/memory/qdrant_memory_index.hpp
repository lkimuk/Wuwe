#ifndef WUWE_AGENT_MEMORY_QDRANT_MEMORY_INDEX_HPP
#define WUWE_AGENT_MEMORY_QDRANT_MEMORY_INDEX_HPP

#include <memory>
#include <string>

#include <wuwe/agent/memory/vector_memory_index.hpp>
#include <wuwe/net/http_client.h>

namespace wuwe::agent::memory {

struct qdrant_memory_index_config {
  std::string base_url { "http://localhost:6333" };
  std::string api_key;
  std::string collection_name { "wuwe_memory" };
  std::string vector_name;
  std::string distance { "Cosine" };
  std::string embedding_provider;
  std::string embedding_model;
  std::string embedding_version;
  std::string index_schema_version { "1" };
  int timeout { 30000 };
  bool create_collection_if_missing { true };
};

class qdrant_memory_index final : public vector_memory_index {
public:
  explicit qdrant_memory_index(qdrant_memory_index_config config);
  qdrant_memory_index(qdrant_memory_index_config config, std::shared_ptr<::wuwe::http_client> http);

  void upsert(const memory_record& record, const std::vector<float>& embedding) override;
  void upsert_batch(
    const std::vector<memory_record>& records,
    const std::vector<std::vector<float>>& embeddings) override;

  std::vector<vector_memory_match> search(const vector_memory_query& query) const override;

  bool erase(const std::string& memory_id, const memory_scope& scope) override;

  std::size_t clear(const memory_scope& scope) override;

private:
  void ensure_collection(std::size_t vector_size) const;
  std::string endpoint(std::string path) const;

private:
  qdrant_memory_index_config config_;
  std::shared_ptr<::wuwe::http_client> http_;
  mutable bool collection_checked_ { false };
};

} // namespace wuwe::agent::memory

#endif // WUWE_AGENT_MEMORY_QDRANT_MEMORY_INDEX_HPP
