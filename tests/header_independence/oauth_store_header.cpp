#include <wuwe/agent/auth/oauth_credential_store.h>

bool oauth_store_header_is_independent() {
  auto store = wuwe::make_memory_oauth_credential_store();
  return store->load().records.empty();
}
