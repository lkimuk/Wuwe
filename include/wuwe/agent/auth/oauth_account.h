#ifndef WUWE_AGENT_AUTH_OAUTH_ACCOUNT_H
#define WUWE_AGENT_AUTH_OAUTH_ACCOUNT_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

#include <wuwe/common/wuwe_fwd.h>

WUWE_NAMESPACE_BEGIN

enum class oauth_error {
  invalid_argument = 1,
  account_not_found,
  reauthentication_required,
  cancelled,
  refresh_failed,
  invalid_response,
  storage_failed,
  storage_corrupt,
  storage_in_use,
  storage_unavailable,
  account_changed,
  account_exists,
  authorization_failed,
  rate_limited,
  timeout,
  limit_exceeded,
};

const std::error_category& oauth_error_category() noexcept;
std::error_code make_error_code(oauth_error value) noexcept;

struct oauth_account_key {
  // Provider namespace and stable local account ID; never a token or email.
  std::string provider;
  std::string account_id;
  bool operator==(const oauth_account_key&) const = default;
};

enum class oauth_account_state { ready, reauthentication_required };

struct oauth_account_info {
  oauth_account_key key;
  std::string display_name;
  // Upstream workspace/account identity, distinct from the local account ID.
  std::string upstream_account_id;
  oauth_account_state state { oauth_account_state::ready };
  // Stable user identity within the upstream issuer, not an email address.
  std::string subject_id;
};

// Sensitive adapter/storage boundary. Do not log or serialize to ordinary app
// configuration. Account listing and normal client configuration use IDs only.
struct oauth_tokens {
  std::string access_token;
  std::string refresh_token;
  std::string id_token;
  std::chrono::system_clock::time_point expires_at;
};

struct oauth_account_record {
  oauth_account_info account;
  oauth_tokens tokens;
};

struct oauth_store_result {
  std::vector<oauth_account_record> records;
  std::error_code error;
};

// Exclusive vault ownership lasts for this object's lifetime. Implementations
// must reject a second owner, preserve the previous snapshot on save failure,
// and durably replace the entire snapshot on success. No network under save().
// The manager serializes load/save; custom stores must not call back into it.
class oauth_credential_store {
public:
  virtual ~oauth_credential_store() = default;
  virtual oauth_store_result load() = 0;
  virtual std::error_code save(const std::vector<oauth_account_record>& records) = 0;
};

struct oauth_refresh_result {
  // Empty refresh_token/id_token preserve their previous values. A successful
  // refresh must supply a nonempty access_token and a future expires_at.
  oauth_tokens tokens;
  std::error_code error;
};

class oauth_token_refresher {
public:
  virtual ~oauth_token_refresher() = default;
  // Bound to a single provider. Implementations must enforce their own network
  // deadline and honor stop; different accounts may call this concurrently.
  virtual std::string provider_id() const = 0;
  virtual oauth_refresh_result refresh(
    const oauth_account_info& account, const oauth_tokens& previous, std::stop_token stop) = 0;
};

struct oauth_access_result {
  // For service adapters only. Previously returned tokens cannot be recalled
  // by local account removal; the manager never silently switches accounts.
  std::string access_token;
  std::chrono::system_clock::time_point expires_at;
  std::error_code error;
  // Atomically paired with the token, for account-scoped service requests.
  oauth_account_info account;
  std::uint64_t revision { 0 };
};

struct oauth_account_snapshot {
  std::optional<oauth_account_info> account;
  std::uint64_t revision { 0 }; // In-memory login generation, not persisted.
  std::error_code error;
};

struct oauth_account_manager_options {
  std::chrono::seconds refresh_before_expiry { 60 };
  // Bounds token parsing/storage and transient secret copies per account.
  std::size_t max_token_bytes { 256 * 1024 };
  std::size_t max_accounts { 256 };
};

class oauth_account_manager {
public:
  // Throws system_error on corrupt/unavailable storage, invalid_argument for
  // invalid dependencies/options. Construction never overwrites a broken vault.
  explicit oauth_account_manager(std::unique_ptr<oauth_credential_store> store,
    std::vector<std::shared_ptr<oauth_token_refresher>> refreshers,
    oauth_account_manager_options options = {});
  ~oauth_account_manager();
  oauth_account_manager(const oauth_account_manager&) = delete;
  oauth_account_manager& operator=(const oauth_account_manager&) = delete;

  std::vector<oauth_account_info> accounts() const;
  oauth_account_snapshot account_snapshot(const oauth_account_key& key) const;
  // Called by a trusted authorization adapter after verifying account identity.
  // Replacement invalidates any refresh started from the old login generation.
  // expected_revision=0 means insert only; a nonzero value is obtained from
  // account_snapshot(). Conditional saves reject duplicate upstream identities.
  std::error_code save_authorization(oauth_account_info account, oauth_tokens tokens,
    std::optional<std::uint64_t> expected_revision = std::nullopt);
  // Local removal only, NOT upstream revocation. Successful removal fences off
  // in-flight refresh results; failed persistence leaves the account unchanged.
  std::error_code remove_account(const oauth_account_key& key);
  // One refresh per account at a time. Waiters can cancel independently. The
  // initiating caller's cancellation can cancel the shared refresh attempt.
  oauth_access_result access_token(const oauth_account_key& key, std::stop_token stop = {});

private:
  class impl;
  std::unique_ptr<impl> impl_;
};

WUWE_NAMESPACE_END

template<>
struct std::is_error_code_enum<wuwe::oauth_error> : std::true_type {};

#endif
