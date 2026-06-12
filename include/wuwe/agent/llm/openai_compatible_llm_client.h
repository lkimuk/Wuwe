#ifndef WUWE_AGENT_LLM_OPENAI_COMPATIBLE_LLM_CLIENT_H
#define WUWE_AGENT_LLM_OPENAI_COMPATIBLE_LLM_CLIENT_H

#include <memory>
#include <stop_token>

#include <nlohmann/json.hpp>

#include <wuwe/agent/llm/llm_client.h>
#include <wuwe/agent/llm/llm_config.h>
#include <wuwe/common/wuwe_fwd.h>
#include <wuwe/net/http_client.h>

WUWE_NAMESPACE_BEGIN

using json = nlohmann::json;

class http_client;

class openai_compatible_llm_client : public llm_client {
public:
  explicit openai_compatible_llm_client(llm_client_config config);
  openai_compatible_llm_client(llm_client_config config, std::shared_ptr<http_client> http);

  llm_response complete(const llm_request& request) override;
  bool supports_streaming() const noexcept override {
    return true;
  }
  llm_response complete(const llm_request& request, std::stop_token stop_token) override;
  llm_response complete_stream(
    const llm_request& request,
    const llm_stream_callbacks& callbacks,
    std::stop_token stop_token = {}) override;

protected:
  static llm_client_config normalize_config(llm_client_config config);

  json build_openai_payload(const llm_request& request) const;
  std::vector<std::pair<std::string, std::string>> build_headers() const;
  llm_response parse_openai_response(const http_response& response) const;

protected:
  llm_client_config config_;
  std::shared_ptr<http_client> http_;
};

WUWE_NAMESPACE_END

#endif // WUWE_AGENT_LLM_OPENAI_COMPATIBLE_LLM_CLIENT_H
