#ifndef WUWE_AGENT_AUTH_CODEX_OAUTH_H
#define WUWE_AGENT_AUTH_CODEX_OAUTH_H

#include <wuwe/agent/auth/oauth_authorization.h>
#include <wuwe/agent/llm/llm_model_discovery.h>

WUWE_NAMESPACE_BEGIN
class http_client;

struct codex_oauth_options {
  // Explicit deployment identity; Wuwe does not borrow another app's client ID.
  std::string client_id;
  std::string client_version;
  std::string originator { "codex_cli_rs" };
  int timeout_ms { 15'000 }; // Per HTTP request, including token exchange.
  std::size_t max_response_bytes { 1024 * 1024 };
  std::size_t max_models { 10'000 };
  std::size_t max_login_sessions { 16 };
};

// Construct before the manager and register with it. Shares the Codex protocol
// with login, including JWT expiry fallback and account identity validation.
std::shared_ptr<oauth_token_refresher> make_codex_oauth_refresher(
  codex_oauth_options options, std::shared_ptr<http_client> http = {});

struct codex_model_list_result {
  // Complete catalog, deduplicated in first-seen order; empty on any error.
  // Catalog membership is not a guarantee of quota or generation entitlement.
  std::vector<llm_model_info> models;
  std::error_code error;
  int http_status { 0 };
};

// Synchronous, thread-safe service adapter for provider "codex_oauth". Manager
// must outlive this object; finish calls before destroying either. An injected
// transport must support concurrent calls, deadlines and stop tokens, and must
// never log credential requests/responses. Fixed HTTPS endpoints, no redirects.
// No browser launching, implicit background polling or credential-file import.
class codex_oauth_client final : public oauth_device_authorizer {
public:
  codex_oauth_client(oauth_account_manager& accounts, codex_oauth_options options,
    std::shared_ptr<http_client> http = {});
  ~codex_oauth_client() override;
  codex_oauth_client(const codex_oauth_client&) = delete;
  codex_oauth_client& operator=(const codex_oauth_client&) = delete;
  std::string provider_id() const override;
  oauth_login_start_result start_login(std::stop_token stop = {}) override;
  // Explicitly replaces this account only; binds user, workspace and generation.
  oauth_login_start_result start_reauthentication(
    const oauth_account_key& account, std::stop_token stop = {});
  oauth_login_poll_result poll_login(
    std::string_view session_id, std::stop_token stop = {}) override;
  std::error_code cancel_login(std::string_view session_id) override;
  // Account token and workspace are obtained together; late results are rejected
  // after logout/re-login. No automatic retry or fallback to API-key discovery.
  codex_model_list_result list_models(const oauth_account_key& account, std::stop_token stop = {});

private:
  class impl;
  std::unique_ptr<impl> impl_;
};
WUWE_NAMESPACE_END
#endif
