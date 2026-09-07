#ifndef WUWE_AGENT_LLM_OPENAI_COMPATIBLE_LLM_CLIENT_H
#define WUWE_AGENT_LLM_OPENAI_COMPATIBLE_LLM_CLIENT_H

#include <memory>
#include <atomic>
#include <stop_token>

#include <nlohmann/json.hpp>

#include <wuwe/agent/llm/llm_client.h>
#include <wuwe/agent/llm/llm_config.h>
#include <wuwe/common/wuwe_fwd.h>
#include <wuwe/net/http_client.h>

WUWE_NAMESPACE_BEGIN

using json = nlohmann::json;

class http_client;

struct openai_compatibility_policy {
  // Some OpenAI-compatible reasoning models expose tool calls through a
  // provider text protocol instead of the standard tool_calls field.
  bool normalize_dsml_tool_calls { false };
  // Filter provider text tool protocol markers at the streaming boundary so
  // protocol markup cannot escape while ordinary text remains incremental.
  bool buffer_text_tool_protocol { false };
  // Retry once without an explicit required/named tool choice when a provider
  // reports that the current reasoning mode does not support it.
  bool negotiate_explicit_tool_choice { false };
  // Provider requires its visible reasoning state to be replayed on the
  // assistant tool-call message in the next request.
  bool replay_reasoning_content { false };
  // Emit the OpenAI-compatible `thinking.type` request control used by
  // providers such as DeepSeek for lightweight non-reasoning subrequests.
  bool request_thinking_control { false };
};

class openai_compatible_llm_client : public llm_client {
public:
  explicit openai_compatible_llm_client(llm_client_config config);
  openai_compatible_llm_client(llm_client_config config, std::shared_ptr<http_client> http);

  llm_response complete(const llm_request& request) override;
  bool supports_streaming() const noexcept override {
    return true;
  }
  [[nodiscard]] llm_provider_capabilities capabilities() const noexcept override {
    return config_.capabilities_override.value_or(llm_provider_capabilities {
      .streaming = true,
      .tools = true,
      .tool_choice = true,
      .json_response_format = true,
      .stop_sequences = true,
    });
  }
  llm_response complete(const llm_request& request, std::stop_token stop_token) override;
  llm_response complete_stream(const llm_request& request, const llm_stream_callbacks& callbacks,
    std::stop_token stop_token = {}) override;

protected:
  openai_compatible_llm_client(llm_client_config config,
    std::shared_ptr<http_client> http, openai_compatibility_policy policy);
  static llm_client_config normalize_config(llm_client_config config);

  json build_openai_payload(const llm_request& request) const;
  std::vector<std::pair<std::string, std::string>> build_headers() const;
  llm_response parse_openai_response(const http_response& response) const;
  llm_response normalize_provider_response(
    const llm_request& request, llm_response response) const;

protected:
  llm_client_config config_;
  std::shared_ptr<http_client> http_;
  openai_compatibility_policy compatibility_policy_;
  mutable std::atomic<bool> explicit_tool_choice_unsupported_ { false };
};

WUWE_NAMESPACE_END

#endif // WUWE_AGENT_LLM_OPENAI_COMPATIBLE_LLM_CLIENT_H
