#include <wuwe/agent/auth/oauth_authorization.h>

bool oauth_authorization_header_is_independent() {
  wuwe::oauth_login_poll_result result;
  return result.state == wuwe::oauth_login_state::pending && !result.account;
}
