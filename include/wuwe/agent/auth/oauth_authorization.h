#ifndef WUWE_AGENT_AUTH_OAUTH_AUTHORIZATION_H
#define WUWE_AGENT_AUTH_OAUTH_AUTHORIZATION_H

#include <optional>
#include <wuwe/agent/auth/oauth_account.h>

WUWE_NAMESPACE_BEGIN

enum class oauth_login_state { pending, slow_down, authorized, denied, expired, cancelled };

struct oauth_device_challenge {
  // Opaque local session handle; never the upstream device_auth_id/verifier.
  std::string session_id;
  std::string verification_uri;
  std::string user_code;
  std::chrono::system_clock::time_point expires_at;
  std::chrono::seconds poll_after { 5 };
};

struct oauth_login_start_result {
  std::optional<oauth_device_challenge> challenge;
  std::error_code error;
};

struct oauth_login_poll_result {
  oauth_login_state state { oauth_login_state::pending };
  // Set only after the authorizer has durably saved the verified account.
  std::optional<oauth_account_info> account;
  std::chrono::seconds poll_after { 5 };
  // Transient/transport failures leave state pending; terminal states are
  // explicit. No tokens or upstream response bodies in UI-facing results.
  std::error_code error;
};

// Implemented by codex_oauth_client. This interface models
// device authorization only, not every possible OAuth grant. It never opens a
// browser, sleeps/polls in the background, or imports another app's credentials.
// Implementations enforce expiry and minimum polling intervals internally.
// Cancellation fences off late responses even when the transport cannot stop.
class oauth_device_authorizer {
public:
  virtual ~oauth_device_authorizer() = default;
  virtual std::string provider_id() const = 0;
  virtual oauth_login_start_result start_login(std::stop_token stop = {}) = 0;
  virtual oauth_login_poll_result poll_login(
    std::string_view session_id, std::stop_token stop = {}) = 0;
  virtual std::error_code cancel_login(std::string_view session_id) = 0;
};

WUWE_NAMESPACE_END
#endif
