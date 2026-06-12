#ifndef WUWE_AGENT_LLM_OLLAMA_LLM_CLIENT_H
#define WUWE_AGENT_LLM_OLLAMA_LLM_CLIENT_H

#include <memory>
#include <stop_token>

#include <nlohmann/json.hpp>

#include <wuwe/agent/llm/llm_client.h>
#include <wuwe/agent/llm/llm_config.h>
#include <wuwe/common/wuwe_fwd.h>
#include <wuwe/net/http_client.h>

WUWE_NAMESPACE_BEGIN

class http_client;

class ollama_llm_client final : public llm_client {
public:
  explicit ollama_llm_client(llm_client_config config);
  ollama_llm_client(llm_client_config config, std::shared_ptr<http_client> http);

  llm_response complete(const llm_request& request) override;
  llm_response complete(const llm_request& request, std::stop_token stop_token) override;
  bool supports_streaming() const noexcept override {
    return true;
  }
  llm_response complete_stream(
    const llm_request& request,
    const llm_stream_callbacks& callbacks,
    std::stop_token stop_token = {}) override;

private:
  static llm_client_config normalize_config(llm_client_config config);

  nlohmann::json build_payload(const llm_request& request, bool stream) const;
  std::vector<std::pair<std::string, std::string>> build_headers() const;
  llm_response parse_response(const http_response& response) const;

  llm_client_config config_;
  std::shared_ptr<http_client> http_;
};

WUWE_NAMESPACE_END

#endif // WUWE_AGENT_LLM_OLLAMA_LLM_CLIENT_H
