#include <wuwe/agent/memory/qdrant_memory_index.hpp>

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <nlohmann/json.hpp>

#include <wuwe/net/default_http_client.h>
#include <wuwe/net/http_status_code.h>

namespace wuwe::agent::memory {

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

std::string qdrant_point_id(const memory_record& record) {
  const std::string key = record.scope.tenant_id + '\n' + record.scope.user_id + '\n' +
                          record.scope.application_id + '\n' + record.scope.conversation_id +
                          '\n' + record.scope.agent_id + '\n' + record.id;
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

json scope_payload(const memory_scope& scope) {
  return {
    { "tenant_id", scope.tenant_id },
    { "user_id", scope.user_id },
    { "application_id", scope.application_id },
    { "conversation_id", scope.conversation_id },
    { "agent_id", scope.agent_id },
  };
}

json record_payload(
  const memory_record& record,
  const qdrant_memory_index_config& config,
  std::size_t embedding_dimension) {
  json payload = scope_payload(record.scope);
  payload["memory_id"] = record.id;
  payload["kind"] = to_string(record.kind);
  payload["visibility"] = to_string(record.visibility);
  payload["priority"] = record.priority;
  payload["created_at"] = vector_detail::to_unix_millis(record.created_at);
  payload["updated_at"] = vector_detail::to_unix_millis(record.updated_at);
  payload["embedding_dimension"] = embedding_dimension;
  payload["index_schema_version"] = config.index_schema_version;
  if (!config.embedding_provider.empty()) {
    payload["embedding_provider"] = config.embedding_provider;
  }
  if (!config.embedding_model.empty()) {
    payload["embedding_model"] = config.embedding_model;
  }
  if (!config.embedding_version.empty()) {
    payload["embedding_version"] = config.embedding_version;
  }
  payload["metadata"] = record.metadata;
  if (record.expires_at) {
    payload["expires_at"] = vector_detail::to_unix_millis(*record.expires_at);
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

json query_filter(const vector_memory_query& query) {
  json must = json::array();
  add_match_condition(must, "tenant_id", query.scope.tenant_id);
  add_match_condition(must, "user_id", query.scope.user_id);
  add_match_condition(must, "application_id", query.scope.application_id);
  add_match_condition(must, "conversation_id", query.scope.conversation_id);
  add_match_condition(must, "agent_id", query.scope.agent_id);

  if (query.kinds.size() == 1) {
    add_match_condition(must, "kind", to_string(query.kinds.front()));
  }
  else if (!query.kinds.empty()) {
    json any = json::array();
    for (const auto kind : query.kinds) {
      any.push_back(to_string(kind));
    }
    must.push_back({
      { "key", "kind" },
      { "match", { { "any", std::move(any) } } },
    });
  }

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
      std::string("qdrant memory index failed to ") + action + ": " +
      response.error_code.message());
  }
}

std::string memory_id_from_payload(const json& payload) {
  if (payload.is_object() && payload.contains("memory_id") && payload["memory_id"].is_string()) {
    return payload["memory_id"].get<std::string>();
  }
  return {};
}

} // namespace

qdrant_memory_index::qdrant_memory_index(qdrant_memory_index_config config)
    : qdrant_memory_index(std::move(config), std::make_shared<::wuwe::default_http_client>()) {
}

qdrant_memory_index::qdrant_memory_index(
  qdrant_memory_index_config config,
  std::shared_ptr<::wuwe::http_client> http)
    : config_(std::move(config)), http_(std::move(http)) {
  if (!http_) {
    throw std::invalid_argument("qdrant_memory_index requires an http_client");
  }
  config_.base_url = trim_trailing_slash(std::move(config_.base_url));
  if (config_.collection_name.empty()) {
    throw std::invalid_argument("qdrant_memory_index requires collection_name");
  }
}

void qdrant_memory_index::upsert(
  const memory_record& record,
  const std::vector<float>& embedding) {
  upsert_batch({ record }, { embedding });
}

void qdrant_memory_index::upsert_batch(
  const std::vector<memory_record>& records,
  const std::vector<std::vector<float>>& embeddings) {
  if (records.size() != embeddings.size()) {
    throw std::invalid_argument("qdrant_memory_index upsert_batch size mismatch");
  }
  if (records.empty()) {
    return;
  }
  const auto vector_size = embeddings.front().size();
  if (vector_size == 0) {
    throw std::invalid_argument("qdrant_memory_index cannot upsert an empty embedding");
  }
  for (const auto& embedding : embeddings) {
    if (embedding.size() != vector_size) {
      throw std::invalid_argument("qdrant_memory_index requires consistent embedding dimensions");
    }
  }

  ensure_collection(vector_size);

  json points = json::array();
  for (std::size_t index = 0; index < records.size(); ++index) {
    json vector = embedding_json(embeddings[index]);
    if (!config_.vector_name.empty()) {
      vector = { { config_.vector_name, std::move(vector) } };
    }

    points.push_back({
      { "id", qdrant_point_id(records[index]) },
      { "vector", std::move(vector) },
      { "payload", record_payload(records[index], config_, vector_size) },
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

std::vector<vector_memory_match> qdrant_memory_index::search(
  const vector_memory_query& query) const {
  if (query.embedding.empty()) {
    return {};
  }

  json vector = embedding_json(query.embedding);
  if (!config_.vector_name.empty()) {
    vector = {
      { "name", config_.vector_name },
      { "vector", std::move(vector) },
    };
  }

  json body {
    { "vector", std::move(vector) },
    { "limit", query.limit },
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
    throw std::runtime_error("qdrant memory index received an invalid search response");
  }

  std::vector<vector_memory_match> matches;
  for (const auto& item : data["result"]) {
    const auto memory_id = memory_id_from_payload(item.value("payload", json::object()));
    if (memory_id.empty()) {
      continue;
    }
    matches.push_back({
      .memory_id = memory_id,
      .score = item.value("score", 0.0),
    });
  }
  return matches;
}

bool qdrant_memory_index::erase(const std::string& memory_id, const memory_scope& scope) {
  vector_memory_query query;
  query.scope = scope;

  auto filter = query_filter(query);
  if (!filter.contains("must")) {
    filter["must"] = json::array();
  }
  add_match_condition(filter["must"], "memory_id", memory_id);

  const json body {
    { "filter", std::move(filter) },
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
  throw_http_error("erase points", response);
  return true;
}

std::size_t qdrant_memory_index::clear(const memory_scope& scope) {
  vector_memory_query query;
  query.scope = scope;

  const json body {
    { "filter", query_filter(query) },
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
  return 0;
}

void qdrant_memory_index::ensure_collection(std::size_t vector_size) const {
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

std::string qdrant_memory_index::endpoint(std::string path) const {
  if (path.empty() || path.front() != '/') {
    path.insert(path.begin(), '/');
  }
  return config_.base_url + path;
}

} // namespace wuwe::agent::memory
