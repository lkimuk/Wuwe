#include <wuwe/agent/auth/oauth_refresh_client.h>

bool oauth_refresh_header_is_independent() {
  wuwe::oauth_refresh_client client({ "test", "https://auth.example/token", "client" });
  std::stop_source stop;
  stop.request_stop();
  return client.refresh({}, {}, stop.get_token()).error == wuwe::oauth_error::cancelled;
}
