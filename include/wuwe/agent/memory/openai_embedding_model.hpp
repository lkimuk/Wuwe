#ifndef WUWE_AGENT_MEMORY_OPENAI_EMBEDDING_MODEL_HPP
#define WUWE_AGENT_MEMORY_OPENAI_EMBEDDING_MODEL_HPP

#include <memory>
#include <string>

#include <wuwe/agent/memory/embedding_model.hpp>
#include <wuwe/net/http_client.h>

namespace wuwe::agent::memory {

struct openai_embedding_model_config {
  std::string base_url;
  std::string api_key;
  std::string model;
  int timeout { 30000 };
  int max_retries { 2 };
  int retry_backoff_ms { 600 };
};

class openai_embedding_model final : public embedding_model {
public:
  explicit openai_embedding_model(openai_embedding_model_config config);
  openai_embedding_model(
    openai_embedding_model_config config,
    std::shared_ptr<::wuwe::http_client> http);

  std::vector<float> embed(std::string_view text) const override;
  std::vector<std::vector<float>> embed_batch(
    const std::vector<std::string>& texts) const override;

private:
  std::vector<std::vector<float>> parse_embedding_response(
    const ::wuwe::http_response& response,
    std::size_t expected_count) const;
  std::string endpoint() const;

private:
  openai_embedding_model_config config_;
  std::shared_ptr<::wuwe::http_client> http_;
};

} // namespace wuwe::agent::memory

#endif // WUWE_AGENT_MEMORY_OPENAI_EMBEDDING_MODEL_HPP
