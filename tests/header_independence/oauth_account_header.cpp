#include <wuwe/agent/auth/oauth_account.h>

bool oauth_account_header_is_independent() {
  wuwe::oauth_account_info account { { "provider", "local-id" } };
  return !account.key.account_id.empty() &&
         wuwe::make_error_code(wuwe::oauth_error::cancelled).category() ==
           wuwe::oauth_error_category();
}
