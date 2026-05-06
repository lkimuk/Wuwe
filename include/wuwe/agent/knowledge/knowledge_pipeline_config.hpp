#ifndef WUWE_AGENT_KNOWLEDGE_PIPELINE_CONFIG_HPP
#define WUWE_AGENT_KNOWLEDGE_PIPELINE_CONFIG_HPP

#include <chrono>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include <wuwe/agent/knowledge/file_knowledge_index.hpp>
#include <wuwe/agent/knowledge/file_knowledge_store.hpp>
#include <wuwe/agent/knowledge/in_memory_knowledge_index.hpp>
#include <wuwe/agent/knowledge/in_memory_knowledge_store.hpp>
#include <wuwe/agent/knowledge/knowledge_cache.hpp>
#include <wuwe/agent/knowledge/knowledge_pipeline.hpp>
#include <wuwe/agent/knowledge/knowledge_query_rewriter.hpp>
#include <wuwe/agent/knowledge/remote_vector_knowledge_index.hpp>

namespace wuwe::agent::knowledge {

inline chunking_policy chunking_policy_from_json(const nlohmann::json& json) {
  chunking_policy policy;
  if (!json.is_object()) {
    return policy;
  }
  policy.max_chars = json.value("max_chars", policy.max_chars);
  policy.overlap_chars = json.value("overlap_chars", policy.overlap_chars);
  policy.max_tokens = json.value("max_tokens", policy.max_tokens);
  policy.overlap_tokens = json.value("overlap_tokens", policy.overlap_tokens);
  policy.respect_markdown_headings =
    json.value("respect_markdown_headings", policy.respect_markdown_headings);
  policy.prefer_paragraph_boundaries =
    json.value("prefer_paragraph_boundaries", policy.prefer_paragraph_boundaries);
  policy.protect_markdown_code_fences =
    json.value("protect_markdown_code_fences", policy.protect_markdown_code_fences);
  policy.respect_code_symbols =
    json.value("respect_code_symbols", policy.respect_code_symbols);
  policy.include_document_summary_chunk =
    json.value("include_document_summary_chunk", policy.include_document_summary_chunk);
  policy.document_summary_chars =
    json.value("document_summary_chars", policy.document_summary_chars);
  return policy;
}

inline knowledge_policy knowledge_policy_from_json(const nlohmann::json& json) {
  knowledge_policy policy;
  if (!json.is_object()) {
    return policy;
  }
  policy.max_context_chars = json.value("max_context_chars", policy.max_context_chars);
  policy.max_results = json.value("max_results", policy.max_results);
  policy.candidate_results = json.value("candidate_results", policy.candidate_results);
  policy.include_citations = json.value("include_citations", policy.include_citations);
  policy.inject_as_system_message =
    json.value("inject_as_system_message", policy.inject_as_system_message);
  policy.surrounding_chunks_before =
    json.value("surrounding_chunks_before", policy.surrounding_chunks_before);
  policy.surrounding_chunks_after =
    json.value("surrounding_chunks_after", policy.surrounding_chunks_after);
  policy.injection_header = json.value("injection_header", policy.injection_header);
  return policy;
}

inline qdrant_knowledge_index_config qdrant_config_from_json(const nlohmann::json& json) {
  qdrant_knowledge_index_config config;
  config.base_url = json.value("base_url", config.base_url);
  config.collection_name = json.value("collection_name", config.collection_name);
  config.api_key = json.value("api_key", config.api_key);
  config.vector_name = json.value("vector_name", config.vector_name);
  config.distance = json.value("distance", config.distance);
  config.timeout = json.value("timeout_ms", config.timeout);
  config.embedding_provider = json.value("embedding_provider", config.embedding_provider);
  config.embedding_model = json.value("embedding_model", config.embedding_model);
  config.embedding_version = json.value("embedding_version", config.embedding_version);
  config.index_schema_version = json.value("index_schema_version", config.index_schema_version);
  config.create_collection_if_missing =
    json.value("create_collection_if_missing", config.create_collection_if_missing);
  return config;
}

inline remote_vector_knowledge_index_config remote_vector_config_from_json(
  const nlohmann::json& json,
  std::string provider) {
  remote_vector_knowledge_index_config config;
  config.base_url = json.value("base_url", config.base_url);
  config.namespace_name = json.value("namespace", config.namespace_name);
  config.provider = std::move(provider);
  config.api_key = json.value("api_key", config.api_key);
  config.timeout_ms = json.value("timeout_ms", config.timeout_ms);
  return config;
}

inline knowledge_pipeline build_knowledge_pipeline_from_json(
  const nlohmann::json& config,
  std::shared_ptr<::wuwe::agent::memory::embedding_model> embedding_model) {
  if (!config.is_object()) {
    throw std::invalid_argument("knowledge pipeline config must be a JSON object");
  }
  if (!embedding_model) {
    throw std::invalid_argument("knowledge pipeline config requires embedding model");
  }

  auto builder = knowledge_pipeline::make()
                   .with_embedding_model(std::move(embedding_model))
                   .with_splitter(knowledge_splitter(
                     chunking_policy_from_json(config.value("chunking", nlohmann::json::object()))))
                   .with_context_policy(
                     knowledge_policy_from_json(config.value("context", nlohmann::json::object())));

  const auto backend = config.value("backend", std::string("local"));
  if (backend == "local") {
    builder.local();
  }
  else if (backend == "file") {
    const auto store_path = config.value("store_path", std::string("knowledge-store.jsonl"));
    const auto index_path = config.value("index_path", std::string("knowledge-index.jsonl"));
    builder.file_backed(store_path, index_path);
  }
  else if (backend == "sqlite") {
    const auto store_path = config.value("store_path", std::string("knowledge-store.jsonl"));
    const auto index_path = config.value("index_path", std::string("knowledge-index.db"));
    builder.sqlite_index(store_path, index_path);
  }
  else if (backend == "qdrant") {
    const auto store_path = config.value("store_path", std::string("knowledge-store.jsonl"));
    builder.qdrant_index(
      store_path,
      qdrant_config_from_json(config.value("qdrant", nlohmann::json::object())));
  }
  else if (backend == "pgvector" || backend == "opensearch" || backend == "milvus") {
    auto remote_config = remote_vector_config_from_json(
      config.value("remote_vector", nlohmann::json::object()),
      backend);
    std::shared_ptr<knowledge_index> index;
    if (backend == "pgvector") {
      index = std::make_shared<pgvector_knowledge_index>(std::move(remote_config));
    }
    else if (backend == "opensearch") {
      index = std::make_shared<opensearch_knowledge_index>(std::move(remote_config));
    }
    else {
      index = std::make_shared<milvus_knowledge_index>(std::move(remote_config));
    }
    builder
      .with_store(std::make_shared<file_knowledge_store>(
        config.value("store_path", std::string("knowledge-store.jsonl"))))
      .with_index(std::move(index));
  }
  else {
    throw std::invalid_argument("unknown knowledge pipeline backend: " + backend);
  }

  auto pipeline = builder.build();
  if (const auto cache = config.value("cache", nlohmann::json::object()); cache.is_object()) {
    if (cache.value("enabled", false)) {
      pipeline.retriever().set_retrieval_cache(
        std::make_shared<in_memory_knowledge_retrieval_cache>(
          cache.value("max_entries", std::size_t { 1024 }),
          std::chrono::milliseconds(cache.value("ttl_ms", 0))));
    }
  }

  if (const auto reranker = config.value("reranker", nlohmann::json::object());
      reranker.is_object()) {
    const auto type = reranker.value("type", std::string {});
    if (type == "bm25") {
      pipeline.retriever().set_reranker(std::make_shared<bm25_knowledge_reranker>());
    }
    else if (type == "mmr") {
      pipeline.retriever().set_reranker(std::make_shared<mmr_knowledge_reranker>());
    }
    else if (!type.empty() && type != "none") {
      throw std::invalid_argument("unknown knowledge reranker type: " + type);
    }
  }

  if (const auto rewrite = config.value("query_rewrite", nlohmann::json::object());
      rewrite.is_object() && rewrite.value("enabled", false)) {
    const auto endpoint = rewrite.value("endpoint_url", std::string {});
    if (!endpoint.empty()) {
      pipeline.retriever().set_query_rewriter(
        std::make_shared<http_knowledge_query_rewriter>(
          http_knowledge_query_rewriter_config {
            .endpoint_url = endpoint,
            .api_key = rewrite.value("api_key", std::string {}),
            .timeout_ms = rewrite.value("timeout_ms", 30000),
            .max_rewrites = rewrite.value("max_rewrites", std::size_t { 4 }),
          }));
    }
  }

  return pipeline;
}

} // namespace wuwe::agent::knowledge

#endif // WUWE_AGENT_KNOWLEDGE_PIPELINE_CONFIG_HPP
