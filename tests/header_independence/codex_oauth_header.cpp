#include <wuwe/agent/auth/codex_oauth.h>

bool codex_oauth_header_is_independent() {
  wuwe::codex_oauth_options options;
  return options.timeout_ms > 0 && options.max_login_sessions > 0;
}
