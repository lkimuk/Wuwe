#ifndef WUWE_AGENT_KNOWLEDGE_REMOTE_VECTOR_KNOWLEDGE_INDEX_HPP
#define WUWE_AGENT_KNOWLEDGE_REMOTE_VECTOR_KNOWLEDGE_INDEX_HPP

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <wuwe/agent/knowledge/file_knowledge_store.hpp>
#include <wuwe/agent/knowledge/in_memory_knowledge_index.hpp>
#include <wuwe/agent/knowledge/knowledge_index.hpp>
#include <wuwe/net/default_http_client.h>
#include <wuwe/net/http_client.h>

namespace wuwe::agent::knowledge {

struct remote_vector_knowledge_index_config {
  std::string base_url;
  std::string namespace_name { "wuwe_knowledge" };
  std::string provider;
  std::string api_key;
  int timeout_ms { 30000 };
};

class remote_vector_knowledge_index : public knowledge_index {
public:
  remote_vector_knowledge_index(
    remote_vector_knowledge_index_config config,
    std::shared_ptr<::wuwe::http_client> http = std::make_shared<::wuwe::default_http_client>())
      : config_(std::move(config)), http_(std::move(http)) {
    if (!http_) {
      throw std::invalid_argument("remote_vector_knowledge_index requires an http_client");
    }
    if (config_.base_url.empty()) {
      throw std::invalid_argument("remote_vector_knowledge_index requires base_url");
    }
    while (!config_.base_url.empty() && config_.base_url.back() == '/') {
      config_.base_url.pop_back();
    }
  }

  void upsert(const knowledge_chunk& chunk, const std::vector<float>& embedding) override {
    upsert_batch({ chunk }, { embedding });
  }

  void upsert_batch(
    const std::vector<knowledge_chunk>& chunks,
    const std::vector<std::vector<float>>& embeddings) override {
    if (chunks.size() != embeddings.size()) {
      throw std::invalid_argument("remote_vector_knowledge_index upsert_batch size mismatch");
    }
    nlohmann::json records = nlohmann::json::array();
    for (std::size_t index = 0; index < chunks.size(); ++index) {
      records.push_back({
        { "chunk", detail::knowledge_chunk_to_json(chunks[index]) },
        { "embedding", embeddings[index] },
      });
    }
    send_json("POST", "/vectors/upsert", {
      { "provider", config_.provider },
      { "namespace", config_.namespace_name },
      { "records", std::move(records) },
    });
  }

  std::vector<knowledge_result> search(
    const knowledge_query& query,
    const std::vector<float>& embedding) const override {
    const auto response = send_json("POST", "/vectors/search", {
      { "provider", config_.provider },
      { "namespace", config_.namespace_name },
      { "query", query.text },
      { "limit", query.limit == 0 ? 1 : query.limit },
      { "embedding", embedding },
      { "filters", query.filters },
    });

    const auto data = nlohmann::json::parse(response.body, nullptr, false);
    if (data.is_discarded()) {
      throw std::runtime_error("remote vector knowledge index received invalid JSON");
    }

    const auto& items = data.contains("results") ? data["results"] : data;
    if (!items.is_array()) {
      throw std::runtime_error("remote vector knowledge index expected array results");
    }

    std::vector<knowledge_result> results;
    for (const auto& item : items) {
      if (!item.contains("chunk")) {
        continue;
      }
      auto chunk = detail::knowledge_chunk_from_json(item["chunk"]);
      if (!metadata_matches(chunk.metadata, query.filters) ||
          !metadata_access_matches(chunk.metadata, query.access)) {
        continue;
      }
      const auto vector_score = item.value("score", 0.0);
      const auto lexical_score = detail::lexical_knowledge_score(query.text, chunk);
      const auto score =
        query.vector_weight * vector_score + query.lexical_weight * lexical_score;
      if (score < query.minimum_score) {
        continue;
      }
      results.push_back({
        .chunk = std::move(chunk),
        .score = score,
        .vector_score = vector_score,
        .lexical_score = lexical_score,
      });
    }

    std::sort(results.begin(), results.end(), [](const knowledge_result& lhs,
                                                  const knowledge_result& rhs) {
      if (lhs.score != rhs.score) {
        return lhs.score > rhs.score;
      }
      return lhs.chunk.id < rhs.chunk.id;
    });
    if (query.limit != 0 && results.size() > query.limit) {
      results.resize(query.limit);
    }
    return results;
  }

  bool erase_document(const std::string& document_id) override {
    send_json("POST", "/vectors/delete", {
      { "provider", config_.provider },
      { "namespace", config_.namespace_name },
      { "document_id", document_id },
    });
    return true;
  }

  void clear() override {
    send_json("POST", "/vectors/clear", {
      { "provider", config_.provider },
      { "namespace", config_.namespace_name },
    });
  }

protected:
  const remote_vector_knowledge_index_config& config() const noexcept {
    return config_;
  }

private:
  ::wuwe::http_response send_json(
    std::string method,
    std::string path,
    nlohmann::json body) const {
    ::wuwe::http_request request {
      .method = std::move(method),
      .url = config_.base_url + std::move(path),
      .headers = { { "Content-Type", "application/json" } },
      .body = body.dump(),
      .timeout = config_.timeout_ms,
    };
    if (!config_.api_key.empty()) {
      request.headers.push_back({ "Authorization", "Bearer " + config_.api_key });
    }
    auto response = http_->send(request);
    if (response.error_code) {
      throw std::runtime_error(
        "remote vector knowledge index request failed: " + response.error_code.message());
    }
    return response;
  }

  remote_vector_knowledge_index_config config_;
  std::shared_ptr<::wuwe::http_client> http_;
};

class pgvector_knowledge_index final : public remote_vector_knowledge_index {
public:
  explicit pgvector_knowledge_index(
    remote_vector_knowledge_index_config config,
    std::shared_ptr<::wuwe::http_client> http = std::make_shared<::wuwe::default_http_client>())
      : remote_vector_knowledge_index(with_provider(std::move(config), "pgvector"), std::move(http)) {
  }

private:
  static remote_vector_knowledge_index_config with_provider(
    remote_vector_knowledge_index_config config,
    std::string provider) {
    config.provider = std::move(provider);
    return config;
  }
};

class opensearch_knowledge_index final : public remote_vector_knowledge_index {
public:
  explicit opensearch_knowledge_index(
    remote_vector_knowledge_index_config config,
    std::shared_ptr<::wuwe::http_client> http = std::make_shared<::wuwe::default_http_client>())
      : remote_vector_knowledge_index(with_provider(std::move(config), "opensearch"), std::move(http)) {
  }

private:
  static remote_vector_knowledge_index_config with_provider(
    remote_vector_knowledge_index_config config,
    std::string provider) {
    config.provider = std::move(provider);
    return config;
  }
};

class milvus_knowledge_index final : public remote_vector_knowledge_index {
public:
  explicit milvus_knowledge_index(
    remote_vector_knowledge_index_config config,
    std::shared_ptr<::wuwe::http_client> http = std::make_shared<::wuwe::default_http_client>())
      : remote_vector_knowledge_index(with_provider(std::move(config), "milvus"), std::move(http)) {
  }

private:
  static remote_vector_knowledge_index_config with_provider(
    remote_vector_knowledge_index_config config,
    std::string provider) {
    config.provider = std::move(provider);
    return config;
  }
};

} // namespace wuwe::agent::knowledge

#endif // WUWE_AGENT_KNOWLEDGE_REMOTE_VECTOR_KNOWLEDGE_INDEX_HPP
