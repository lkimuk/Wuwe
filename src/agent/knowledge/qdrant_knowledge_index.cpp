#include <wuwe/agent/knowledge/qdrant_knowledge_index.hpp>

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <nlohmann/json.hpp>

#include <wuwe/agent/knowledge/file_knowledge_store.hpp>
#include <wuwe/agent/knowledge/in_memory_knowledge_index.hpp>
#include <wuwe/net/default_http_client.h>
#include <wuwe/net/http_status_code.h>

namespace wuwe::agent::knowledge {

namespace {

using json = nlohmann::json;

std::string trim_trailing_slash(std::string value) {
  while (!value.empty() && value.back() == '/') {
    value.pop_back();
  }
  return value;
}

std::uint64_t fnv1a64(std::string_view text, std::uint64_t seed) {
  std::uint64_t hash = seed;
  for (const unsigned char c : text) {
    hash ^= c;
    hash *= 1099511628211ull;
  }
  return hash;
}

std::string qdrant_point_id(const knowledge_chunk& chunk) {
  const std::string key = chunk.document_id + '\n' + chunk.id + '\n' +
                          std::to_string(chunk.start_offset);
  const auto high = fnv1a64(key, 14695981039346656037ull);
  const auto low = fnv1a64(key, 1099511628211ull);

  std::ostringstream output;
  output << std::hex << std::setfill('0')
         << std::setw(8) << static_cast<std::uint32_t>(high >> 32) << '-'
         << std::setw(4) << static_cast<std::uint16_t>(high >> 16) << '-'
         << std::setw(4) << static_cast<std::uint16_t>((high & 0x0fffull) | 0x5000ull) << '-'
         << std::setw(4) << static_cast<std::uint16_t>(((low >> 48) & 0x3fffull) | 0x8000ull)
         << '-'
         << std::setw(12) << (low & 0xffffffffffffull);
  return output.str();
}

json embedding_json(const std::vector<float>& embedding) {
  json result = json::array();
  for (const auto value : embedding) {
    result.push_back(value);
  }
  return result;
}

json chunk_payload(
  const knowledge_chunk& chunk,
  const qdrant_knowledge_index_config& config,
  std::size_t embedding_dimension) {
  json payload {
    { "chunk_id", chunk.id },
    { "document_id", chunk.document_id },
    { "title", chunk.title },
    { "source_uri", chunk.source_uri },
    { "start_offset", chunk.start_offset },
    { "end_offset", chunk.end_offset },
    { "start_line", chunk.start_line },
    { "end_line", chunk.end_line },
    { "embedding_dimension", embedding_dimension },
    { "index_schema_version", config.index_schema_version },
    { "metadata", chunk.metadata },
    { "chunk", detail::knowledge_chunk_to_json(chunk) },
  };
  if (!config.embedding_provider.empty()) {
    payload["embedding_provider"] = config.embedding_provider;
  }
  if (!config.embedding_model.empty()) {
    payload["embedding_model"] = config.embedding_model;
  }
  if (!config.embedding_version.empty()) {
    payload["embedding_version"] = config.embedding_version;
  }

  if (const auto tenant = chunk.metadata.find("tenant_id"); tenant != chunk.metadata.end()) {
    payload["tenant_id"] = tenant->second;
  }
  if (const auto user = chunk.metadata.find("user_id"); user != chunk.metadata.end()) {
    payload["user_id"] = user->second;
  }
  return payload;
}

void add_match_condition(json& must, std::string key, std::string value) {
  if (value.empty()) {
    return;
  }
  must.push_back({
    { "key", std::move(key) },
    { "match", { { "value", std::move(value) } } },
  });
}

json query_filter(const knowledge_query& query) {
  json must = json::array();
  add_match_condition(must, "tenant_id", query.access.tenant_id);
  add_match_condition(must, "user_id", query.access.user_id);

  for (const auto& [key, value] : query.filters) {
    add_match_condition(must, "metadata." + key, value);
  }

  if (must.empty()) {
    return json::object();
  }
  return { { "must", std::move(must) } };
}

void throw_http_error(const char* action, const ::wuwe::http_response& response) {
  if (response.error_code) {
    throw std::runtime_error(
      std::string("qdrant knowledge index failed to ") + action + ": " +
      response.error_code.message());
  }
}

std::optional<knowledge_chunk> chunk_from_payload(const json& payload) {
  if (!payload.is_object() || !payload.contains("chunk") || !payload["chunk"].is_object()) {
    return std::nullopt;
  }
  return detail::knowledge_chunk_from_json(payload["chunk"]);
}

} // namespace

qdrant_knowledge_index::qdrant_knowledge_index(qdrant_knowledge_index_config config)
    : qdrant_knowledge_index(std::move(config), std::make_shared<::wuwe::default_http_client>()) {
}

qdrant_knowledge_index::qdrant_knowledge_index(
  qdrant_knowledge_index_config config,
  std::shared_ptr<::wuwe::http_client> http)
    : config_(std::move(config)), http_(std::move(http)) {
  if (!http_) {
    throw std::invalid_argument("qdrant_knowledge_index requires an http_client");
  }
  config_.base_url = trim_trailing_slash(std::move(config_.base_url));
  if (config_.collection_name.empty()) {
    throw std::invalid_argument("qdrant_knowledge_index requires collection_name");
  }
}

void qdrant_knowledge_index::upsert(
  const knowledge_chunk& chunk,
  const std::vector<float>& embedding) {
  upsert_batch({ chunk }, { embedding });
}

void qdrant_knowledge_index::upsert_batch(
  const std::vector<knowledge_chunk>& chunks,
  const std::vector<std::vector<float>>& embeddings) {
  if (chunks.size() != embeddings.size()) {
    throw std::invalid_argument("qdrant_knowledge_index upsert_batch size mismatch");
  }
  if (chunks.empty()) {
    return;
  }
  const auto vector_size = embeddings.front().size();
  if (vector_size == 0) {
    throw std::invalid_argument("qdrant_knowledge_index cannot upsert an empty embedding");
  }
  for (const auto& embedding : embeddings) {
    if (embedding.size() != vector_size) {
      throw std::invalid_argument("qdrant_knowledge_index requires consistent embedding dimensions");
    }
  }

  ensure_collection(vector_size);

  json points = json::array();
  for (std::size_t index = 0; index < chunks.size(); ++index) {
    json vector = embedding_json(embeddings[index]);
    if (!config_.vector_name.empty()) {
      vector = { { config_.vector_name, std::move(vector) } };
    }

    points.push_back({
      { "id", qdrant_point_id(chunks[index]) },
      { "vector", std::move(vector) },
      { "payload", chunk_payload(chunks[index], config_, vector_size) },
    });
  }

  const json body {
    { "points", std::move(points) },
  };

  ::wuwe::http_request request {
    .method = "PUT",
    .url = endpoint("/collections/" + config_.collection_name + "/points?wait=true"),
    .headers = { { "Content-Type", "application/json" } },
    .body = body.dump(),
    .timeout = config_.timeout,
  };
  if (!config_.api_key.empty()) {
    request.headers.push_back({ "api-key", config_.api_key });
  }

  const auto response = http_->send(request);
  throw_http_error("upsert points", response);
}

std::vector<knowledge_result> qdrant_knowledge_index::search(
  const knowledge_query& query,
  const std::vector<float>& embedding) const {
  if (embedding.empty() || query.text.empty()) {
    return {};
  }

  json vector = embedding_json(embedding);
  if (!config_.vector_name.empty()) {
    vector = {
      { "name", config_.vector_name },
      { "vector", std::move(vector) },
    };
  }

  json body {
    { "vector", std::move(vector) },
    { "limit", query.limit == 0 ? 1 : query.limit },
    { "with_payload", true },
  };

  const auto filter = query_filter(query);
  if (!filter.empty()) {
    body["filter"] = filter;
  }

  ::wuwe::http_request request {
    .method = "POST",
    .url = endpoint("/collections/" + config_.collection_name + "/points/search"),
    .headers = { { "Content-Type", "application/json" } },
    .body = body.dump(),
    .timeout = config_.timeout,
  };
  if (!config_.api_key.empty()) {
    request.headers.push_back({ "api-key", config_.api_key });
  }

  const auto response = http_->send(request);
  throw_http_error("search points", response);

  const auto data = json::parse(response.body, nullptr, false);
  if (data.is_discarded() || !data.contains("result") || !data["result"].is_array()) {
    throw std::runtime_error("qdrant knowledge index received an invalid search response");
  }

  std::vector<knowledge_result> results;
  for (const auto& item : data["result"]) {
    auto chunk = chunk_from_payload(item.value("payload", json::object()));
    if (!chunk || !metadata_matches(chunk->metadata, query.filters) ||
        !metadata_access_matches(chunk->metadata, query.access)) {
      continue;
    }

    const auto vector_score = item.value("score", 0.0);
    const auto lexical_score = detail::lexical_knowledge_score(query.text, *chunk);
    const auto score =
      query.vector_weight * vector_score + query.lexical_weight * lexical_score;
    if (score < query.minimum_score) {
      continue;
    }

    results.push_back({
      .chunk = std::move(*chunk),
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
    if (lhs.chunk.document_id != rhs.chunk.document_id) {
      return lhs.chunk.document_id < rhs.chunk.document_id;
    }
    return lhs.chunk.start_offset < rhs.chunk.start_offset;
  });

  if (query.limit != 0 && results.size() > query.limit) {
    results.resize(query.limit);
  }
  return results;
}

bool qdrant_knowledge_index::erase_document(const std::string& document_id) {
  json must = json::array();
  add_match_condition(must, "document_id", document_id);

  const json body {
    { "filter", { { "must", std::move(must) } } },
  };

  ::wuwe::http_request request {
    .method = "POST",
    .url = endpoint("/collections/" + config_.collection_name + "/points/delete?wait=true"),
    .headers = { { "Content-Type", "application/json" } },
    .body = body.dump(),
    .timeout = config_.timeout,
  };
  if (!config_.api_key.empty()) {
    request.headers.push_back({ "api-key", config_.api_key });
  }

  const auto response = http_->send(request);
  throw_http_error("erase document points", response);
  return true;
}

void qdrant_knowledge_index::clear() {
  const json body {
    { "filter", json::object() },
  };

  ::wuwe::http_request request {
    .method = "POST",
    .url = endpoint("/collections/" + config_.collection_name + "/points/delete?wait=true"),
    .headers = { { "Content-Type", "application/json" } },
    .body = body.dump(),
    .timeout = config_.timeout,
  };
  if (!config_.api_key.empty()) {
    request.headers.push_back({ "api-key", config_.api_key });
  }

  const auto response = http_->send(request);
  throw_http_error("clear points", response);
}

void qdrant_knowledge_index::ensure_collection(std::size_t vector_size) const {
  if (collection_checked_ || !config_.create_collection_if_missing) {
    return;
  }

  ::wuwe::http_request get_request {
    .method = "GET",
    .url = endpoint("/collections/" + config_.collection_name),
    .headers = {},
    .body = {},
    .timeout = config_.timeout,
  };
  if (!config_.api_key.empty()) {
    get_request.headers.push_back({ "api-key", config_.api_key });
  }

  const auto get_response = http_->send(get_request);
  if (!get_response.error_code) {
    collection_checked_ = true;
    return;
  }
  if (get_response.error_code != ::wuwe::http_status_code::not_found) {
    throw_http_error("check collection", get_response);
  }

  json vectors {
    { "size", vector_size },
    { "distance", config_.distance },
  };
  if (!config_.vector_name.empty()) {
    vectors = {
      { config_.vector_name, std::move(vectors) },
    };
  }

  const json body {
    { "vectors", std::move(vectors) },
  };

  ::wuwe::http_request request {
    .method = "PUT",
    .url = endpoint("/collections/" + config_.collection_name),
    .headers = { { "Content-Type", "application/json" } },
    .body = body.dump(),
    .timeout = config_.timeout,
  };
  if (!config_.api_key.empty()) {
    request.headers.push_back({ "api-key", config_.api_key });
  }

  const auto response = http_->send(request);
  throw_http_error("ensure collection", response);
  collection_checked_ = true;
}

std::string qdrant_knowledge_index::endpoint(std::string path) const {
  if (path.empty() || path.front() != '/') {
    path.insert(path.begin(), '/');
  }
  return config_.base_url + path;
}

} // namespace wuwe::agent::knowledge
