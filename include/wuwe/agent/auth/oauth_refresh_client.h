#ifndef WUWE_AGENT_AUTH_OAUTH_REFRESH_CLIENT_H
#define WUWE_AGENT_AUTH_OAUTH_REFRESH_CLIENT_H

#include <wuwe/agent/auth/oauth_account.h>

WUWE_NAMESPACE_BEGIN
class http_client;

struct oauth_refresh_endpoint {
  std::string provider_id;
  // HTTPS token endpoint, no userinfo/query/fragment. Never inferred from a
  // generation base_url; no redirects, retries or endpoint probing.
  std::string token_url;
  std::string client_id;
  int timeout_ms { 15'000 };
  std::size_t max_response_bytes { 1024 * 1024 };
};

// Public-client OAuth refresh_token grant (application/x-www-form-urlencoded).
// Provider-specific grants can implement oauth_token_refresher directly.
class oauth_refresh_client final : public oauth_token_refresher {
public:
  explicit oauth_refresh_client(
    oauth_refresh_endpoint endpoint, std::shared_ptr<http_client> http = nullptr);
  std::string provider_id() const override;
  oauth_refresh_result refresh(
    const oauth_account_info& account, const oauth_tokens& previous, std::stop_token stop) override;

private:
  oauth_refresh_endpoint endpoint_;
  std::shared_ptr<http_client> http_;
};

WUWE_NAMESPACE_END
#endif
