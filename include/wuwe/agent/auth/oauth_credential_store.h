#ifndef WUWE_AGENT_AUTH_OAUTH_CREDENTIAL_STORE_H
#define WUWE_AGENT_AUTH_OAUTH_CREDENTIAL_STORE_H

#include <filesystem>
#include <memory>
#include <wuwe/agent/auth/oauth_account.h>

WUWE_NAMESPACE_BEGIN

// Explicitly ephemeral: destruction discards all credentials, no disk fallback.
std::unique_ptr<oauth_credential_store> make_memory_oauth_credential_store();

// Windows: DPAPI current-user encryption, private ACLs, atomic replacement and
// exclusive vault lock. Path must be absolute in an application-owned local
// directory. Does not read Codex/CCS credentials or migrate other applications.
// Throws system_error on failure; unsupported platforms fail closed. Applications
// on other platforms may inject their own OS-keychain-backed store.
std::unique_ptr<oauth_credential_store> make_system_oauth_credential_store(
  const std::filesystem::path& vault_path);

WUWE_NAMESPACE_END
#endif
