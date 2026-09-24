#ifndef WUWE_AGENT_LLM_CODEX_LLM_CLIENT_H
#define WUWE_AGENT_LLM_CODEX_LLM_CLIENT_H

#include <wuwe/agent/auth/oauth_account.h>
#include <wuwe/agent/llm/llm_client.h>
#include <wuwe/agent/llm/llm_config.h>

WUWE_NAMESPACE_BEGIN
class http_client;

struct codex_llm_options {
  std::string model;          // Request.model overrides this; never guess a model ID.
  std::string client_version; // Same upstream protocol version as model discovery.
  std::string originator { "codex_cli_rs" };
  int timeout_ms { 120'000 }; // Generation request, after obtaining credentials.
  llm_stream_timeout_options stream_timeouts;
  std::size_t max_request_bytes { 8 * 1024 * 1024 };
  std::size_t max_response_bytes { 16 * 1024 * 1024 }; // Entire SSE stream.
  std::size_t max_event_bytes { 2 * 1024 * 1024 };
  std::size_t max_output_items { 1024 };
  std::optional<std::string> reasoning_effort; // none/minimal/low/medium/high/xhigh
  bool reasoning_summary { true };             // Only provider-visible summaries are exposed.
  bool parallel_tool_calls { true };
};

// Account-bound Responses/SSE adapter, not an API-key preset. The manager must
// outlive this object. Calls may run concurrently with a concurrency-safe HTTP
// transport. No automatic generation retries, account switching or tool execution.
// complete() aggregates the same stream decoder used by complete_stream().
class codex_llm_client final : public llm_client {
public:
  codex_llm_client(oauth_account_manager& accounts, oauth_account_key account,
    codex_llm_options options, std::shared_ptr<http_client> http = {});
  ~codex_llm_client() override;
  codex_llm_client(const codex_llm_client&) = delete;
  codex_llm_client& operator=(const codex_llm_client&) = delete;
  using llm_client::complete;
  llm_response complete(const llm_request& request) override;
  llm_response complete(const llm_request& request, std::stop_token stop) override;
  bool supports_streaming() const noexcept override {
    return true;
  }
  llm_provider_capabilities capabilities() const noexcept override;
  // Deltas are provisional. Tool-call completion and done are emitted only
  // after a valid terminal event, HTTP success and account-generation check.
  // Failure returns no executable tool calls or continuation state; already
  // delivered text cannot be recalled. Callback exceptions propagate to caller.
  llm_response complete_stream(const llm_request& request, const llm_stream_callbacks& callbacks,
    std::stop_token stop = {}) override;

private:
  class impl;
  std::unique_ptr<impl> impl_;
};
WUWE_NAMESPACE_END
#endif
