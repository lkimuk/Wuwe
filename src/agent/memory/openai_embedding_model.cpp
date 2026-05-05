#include <wuwe/agent/memory/openai_embedding_model.hpp>

#include <chrono>
#include <algorithm>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>

#include <nlohmann/json.hpp>

#include <wuwe/net/default_http_client.h>
#include <wuwe/net/net_errc.h>

namespace wuwe::agent::memory {

namespace {

using json = nlohmann::json;

std::string trim_trailing_slash(std::string value) {
  while (!value.empty() && value.back() == '/') {
    value.pop_back();
  }
  return value;
}

bool is_retryable_error(const std::error_code& ec) {
  return ec == ::wuwe::net_errc::rate_limited || ec == ::wuwe::net_errc::timeout ||
         ec == ::wuwe::net_errc::connection_failed ||
         ec == ::wuwe::net_errc::transport_failed || ec == ::wuwe::net_errc::server_error ||
         ec == ::wuwe::net_errc::service_unavailable;
}

int compute_backoff_ms(int attempt, int base_backoff_ms) {
  constexpr int max_power = 6;
  const int clamped_attempt = attempt < max_power ? attempt : max_power;
  return base_backoff_ms * (1 << clamped_attempt);
}

} // namespace

openai_embedding_model::openai_embedding_model(openai_embedding_model_config config)
    : openai_embedding_model(std::move(config), std::make_shared<::wuwe::default_http_client>()) {
}

openai_embedding_model::openai_embedding_model(
  openai_embedding_model_config config,
  std::shared_ptr<::wuwe::http_client> http)
    : config_(std::move(config)), http_(std::move(http)) {
  if (!http_) {
    throw std::invalid_argument("openai_embedding_model requires an http_client");
  }
  config_.base_url = trim_trailing_slash(std::move(config_.base_url));
  if (config_.base_url.empty()) {
    throw std::invalid_argument("openai_embedding_model requires base_url");
  }
  if (config_.model.empty()) {
    throw std::invalid_argument("openai_embedding_model requires model");
  }
}

std::vector<float> openai_embedding_model::embed(std::string_view text) const {
  auto embeddings = embed_batch({ std::string(text) });
  if (embeddings.empty()) {
    throw std::runtime_error("openai_embedding_model returned no embeddings");
  }
  return std::move(embeddings.front());
}

std::vector<std::vector<float>> openai_embedding_model::embed_batch(
  const std::vector<std::string>& texts) const {
  if (texts.empty()) {
    return {};
  }

  json input;
  if (texts.size() == 1) {
    input = texts.front();
  }
  else {
    input = json::array();
    for (const auto& text : texts) {
      input.push_back(text);
    }
  }

  const json payload {
    { "model", config_.model },
    { "input", std::move(input) },
  };

  ::wuwe::http_request request {
    .method = "POST",
    .url = endpoint(),
    .headers = { { "Content-Type", "application/json" } },
    .body = payload.dump(),
    .timeout = config_.timeout,
  };
  if (!config_.api_key.empty()) {
    request.headers.push_back({ "Authorization", "Bearer " + config_.api_key });
  }

  const int max_retries = config_.max_retries < 0 ? 0 : config_.max_retries;
  const int base_backoff_ms = config_.retry_backoff_ms <= 0 ? 500 : config_.retry_backoff_ms;

  for (int attempt = 0; attempt <= max_retries; ++attempt) {
    const auto response = http_->send(request);
    try {
      return parse_embedding_response(response, texts.size());
    }
    catch (const std::system_error& ex) {
      if (attempt >= max_retries || !is_retryable_error(ex.code())) {
        throw;
      }
    }
    std::this_thread::sleep_for(
      std::chrono::milliseconds(compute_backoff_ms(attempt, base_backoff_ms)));
  }

  throw std::runtime_error("openai_embedding_model failed to embed text");
}

std::vector<std::vector<float>> openai_embedding_model::parse_embedding_response(
  const ::wuwe::http_response& response,
  std::size_t expected_count) const {
  if (response.error_code) {
    throw std::system_error(response.error_code, "embedding request failed");
  }

  const auto data = json::parse(response.body, nullptr, false);
  if (data.is_discarded() || !data.contains("data") || !data["data"].is_array()) {
    throw std::runtime_error("embedding response does not contain data array");
  }

  std::vector<std::pair<std::size_t, std::vector<float>>> indexed_embeddings;
  indexed_embeddings.reserve(data["data"].size());

  for (const auto& item : data["data"]) {
    if (!item.is_object() || !item.contains("embedding") || !item["embedding"].is_array()) {
      throw std::runtime_error("embedding response item does not contain embedding");
    }

    const auto index = item.value("index", indexed_embeddings.size());
    std::vector<float> embedding;
    for (const auto& value : item["embedding"]) {
      if (!value.is_number()) {
        throw std::runtime_error("embedding response contains a non-numeric vector value");
      }
      embedding.push_back(value.get<float>());
    }
    if (embedding.empty()) {
      throw std::runtime_error("embedding response contains an empty vector");
    }
    indexed_embeddings.push_back({ index, std::move(embedding) });
  }

  if (indexed_embeddings.size() != expected_count) {
    throw std::runtime_error("embedding response count does not match request count");
  }

  std::sort(indexed_embeddings.begin(), indexed_embeddings.end(), [](const auto& lhs, const auto& rhs) {
    return lhs.first < rhs.first;
  });

  std::vector<std::vector<float>> embeddings;
  embeddings.reserve(indexed_embeddings.size());
  for (auto& [_, embedding] : indexed_embeddings) {
    embeddings.push_back(std::move(embedding));
  }
  return embeddings;
}

std::string openai_embedding_model::endpoint() const {
  return config_.base_url + "/v1/embeddings";
}

} // namespace wuwe::agent::memory
