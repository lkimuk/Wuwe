#include <wuwe/agent/auth/oauth_account.h>

#include <algorithm>
#include <condition_variable>
#include <map>
#include <mutex>
#include <stdexcept>
#include <utility>

WUWE_NAMESPACE_BEGIN
namespace {
class auth_category final : public std::error_category {
public:
  const char* name() const noexcept override {
    return "wuwe.oauth";
  }
  std::string message(int code) const override {
    switch (static_cast<oauth_error>(code)) {
      case oauth_error::invalid_argument:
        return "Invalid OAuth argument";
      case oauth_error::account_not_found:
        return "OAuth account not found";
      case oauth_error::reauthentication_required:
        return "OAuth login required";
      case oauth_error::cancelled:
        return "OAuth operation cancelled";
      case oauth_error::refresh_failed:
        return "OAuth token refresh failed";
      case oauth_error::invalid_response:
        return "Invalid OAuth response";
      case oauth_error::storage_failed:
        return "OAuth credential storage failed";
      case oauth_error::storage_corrupt:
        return "OAuth credential storage is corrupt";
      case oauth_error::storage_in_use:
        return "OAuth credential storage is already in use";
      case oauth_error::storage_unavailable:
        return "OAuth secure storage is unavailable";
      case oauth_error::account_changed:
        return "OAuth account changed during operation";
      case oauth_error::account_exists:
        return "OAuth account already exists";
      case oauth_error::authorization_failed:
        return "OAuth authorization failed";
      case oauth_error::rate_limited:
        return "OAuth request rate limited";
      case oauth_error::timeout:
        return "OAuth request timed out";
      case oauth_error::limit_exceeded:
        return "OAuth resource limit exceeded";
    }
    return "Unknown OAuth error";
  }
};

using key_type = std::pair<std::string, std::string>;
key_type index_of(const oauth_account_key& key) {
  return { key.provider, key.account_id };
}
bool valid_text(const std::string& value, std::size_t max, bool required = true) {
  return (!required || !value.empty()) && value.size() <= max &&
         std::none_of(
           value.begin(), value.end(), [](unsigned char c) { return c < 32 || c == 127; });
}
bool valid_account(const oauth_account_info& account) {
  return valid_text(account.key.provider, 128) && valid_text(account.key.account_id, 256) &&
         valid_text(account.display_name, 1024, false) &&
         valid_text(account.upstream_account_id, 1024, false) &&
         valid_text(account.subject_id, 1024, false) &&
         (account.state == oauth_account_state::ready ||
           account.state == oauth_account_state::reauthentication_required);
}
bool valid_tokens(const oauth_tokens& tokens, std::size_t max) {
  return valid_text(tokens.access_token, max) && valid_text(tokens.refresh_token, max, false) &&
         valid_text(tokens.id_token, max, false);
}
} // namespace

const std::error_category& oauth_error_category() noexcept {
  static auth_category category;
  return category;
}
std::error_code make_error_code(oauth_error value) noexcept {
  return { static_cast<int>(value), oauth_error_category() };
}

class oauth_account_manager::impl {
public:
  struct entry {
    std::uint64_t revision { 0 };
    oauth_account_record record;
    bool refreshing { false };
    // A failed rotation write blocks further refreshes in this manager, avoiding
    // reuse of the old refresh token. Re-login/removal is required to recover.
    bool persistence_uncertain { false };
    std::error_code last_error;
    std::condition_variable_any changed;
  };
  mutable std::mutex mutex;
  std::uint64_t next_revision { 1 };
  std::unique_ptr<oauth_credential_store> store;
  std::map<key_type, std::shared_ptr<entry>> entries;
  std::map<std::string, std::shared_ptr<oauth_token_refresher>> refreshers;
  oauth_account_manager_options options;

  std::error_code persist(const key_type& key, const oauth_account_record* replacement) {
    std::vector<oauth_account_record> records;
    for (const auto& [id, value] : entries) {
      if (id != key)
        records.push_back(value->record);
    }
    if (replacement)
      records.push_back(*replacement);
    try {
      return store->save(records);
    }
    catch (...) {
      return make_error_code(oauth_error::storage_failed);
    }
  }
};

oauth_account_manager::oauth_account_manager(std::unique_ptr<oauth_credential_store> store,
  std::vector<std::shared_ptr<oauth_token_refresher>> refreshers,
  oauth_account_manager_options options)
    : impl_(std::make_unique<impl>()) {
  if (!store || options.refresh_before_expiry < std::chrono::seconds::zero() ||
      options.refresh_before_expiry > std::chrono::hours(24) || options.max_token_bytes == 0 ||
      options.max_accounts == 0) {
    throw std::invalid_argument("Invalid OAuth manager configuration");
  }
  impl_->store = std::move(store);
  impl_->options = options;
  for (auto& refresher : refreshers) {
    if (!refresher)
      throw std::invalid_argument("Missing OAuth refresher");
    auto id = refresher->provider_id();
    if (!valid_text(id, 128) || !impl_->refreshers.emplace(id, refresher).second)
      throw std::invalid_argument("Invalid or duplicate OAuth refresher");
  }
  oauth_store_result loaded;
  try {
    loaded = impl_->store->load();
  }
  catch (...) {
    throw std::system_error(make_error_code(oauth_error::storage_failed));
  }
  if (loaded.error)
    throw std::system_error(loaded.error);
  if (loaded.records.size() > options.max_accounts)
    throw std::system_error(make_error_code(oauth_error::storage_corrupt));
  for (auto& record : loaded.records) {
    if (!valid_account(record.account) || !valid_tokens(record.tokens, options.max_token_bytes))
      throw std::system_error(make_error_code(oauth_error::storage_corrupt));
    auto value = std::make_shared<impl::entry>();
    value->revision = impl_->next_revision++;
    const auto key = index_of(record.account.key);
    value->record = std::move(record);
    if (!impl_->entries.emplace(key, std::move(value)).second)
      throw std::system_error(make_error_code(oauth_error::storage_corrupt));
  }
}
oauth_account_manager::~oauth_account_manager() = default;

std::vector<oauth_account_info> oauth_account_manager::accounts() const {
  std::lock_guard lock(impl_->mutex);
  std::vector<oauth_account_info> result;
  for (const auto& [key, value] : impl_->entries)
    result.push_back(value->record.account);
  return result;
}

std::error_code oauth_account_manager::save_authorization(
  oauth_account_info account, oauth_tokens tokens, std::optional<std::uint64_t> expected_revision) {
  if (!valid_account(account) || !valid_tokens(tokens, impl_->options.max_token_bytes) ||
      tokens.expires_at <= std::chrono::system_clock::now())
    return oauth_error::invalid_argument;
  account.state = oauth_account_state::ready;
  const auto key = index_of(account.key);
  auto value = std::make_shared<impl::entry>();
  value->record = { std::move(account), std::move(tokens) };
  std::lock_guard lock(impl_->mutex);
  if (expected_revision) {
    const auto existing = impl_->entries.find(key);
    if (*expected_revision == 0
          ? existing != impl_->entries.end()
          : existing == impl_->entries.end() || existing->second->revision != *expected_revision)
      return oauth_error::account_changed;
    for (const auto& [other_key, other] : impl_->entries) {
      const auto& incoming = value->record.account;
      if (other_key != key && other_key.first == key.first && !incoming.subject_id.empty() &&
          !incoming.upstream_account_id.empty() &&
          other->record.account.subject_id == incoming.subject_id &&
          other->record.account.upstream_account_id == incoming.upstream_account_id)
        return oauth_error::account_exists;
    }
  }
  if (!impl_->entries.contains(key) && impl_->entries.size() >= impl_->options.max_accounts)
    return oauth_error::limit_exceeded;
  if (auto error = impl_->persist(key, &value->record))
    return error;
  value->revision = impl_->next_revision++;
  auto& previous = impl_->entries[key];
  if (previous)
    previous->changed.notify_all();
  previous = std::move(value);
  return {};
}

oauth_account_snapshot oauth_account_manager::account_snapshot(const oauth_account_key& key) const {
  std::lock_guard lock(impl_->mutex);
  const auto at = impl_->entries.find(index_of(key));
  if (at == impl_->entries.end())
    return { {}, 0, oauth_error::account_not_found };
  return { at->second->record.account, at->second->revision, {} };
}

std::error_code oauth_account_manager::remove_account(const oauth_account_key& key) {
  std::lock_guard lock(impl_->mutex);
  const auto found = impl_->entries.find(index_of(key));
  if (found == impl_->entries.end())
    return oauth_error::account_not_found;
  if (auto error = impl_->persist(found->first, nullptr))
    return error;
  found->second->changed.notify_all();
  impl_->entries.erase(found);
  return {};
}

oauth_access_result oauth_account_manager::access_token(
  const oauth_account_key& account, std::stop_token stop) {
  const auto fail = [](std::error_code error) { return oauth_access_result { .error = error }; };
  if (stop.stop_requested())
    return fail(oauth_error::cancelled);
  std::unique_lock lock(impl_->mutex);
  const auto key = index_of(account);
  auto found = impl_->entries.find(key);
  if (found == impl_->entries.end())
    return fail(oauth_error::account_not_found);
  auto value = found->second;
  const auto current = [&] {
    const auto at = impl_->entries.find(key);
    return at != impl_->entries.end() && at->second == value;
  };
  if (value->refreshing) {
    value->changed.wait(lock, stop, [&] { return !current() || !value->refreshing; });
    if (stop.stop_requested())
      return fail(oauth_error::cancelled);
    if (!current())
      return fail(oauth_error::account_changed);
    if (value->last_error)
      return fail(value->last_error);
    if (value->record.tokens.expires_at > std::chrono::system_clock::now())
      return { value->record.tokens.access_token,
        value->record.tokens.expires_at,
        {},
        value->record.account,
        value->revision };
  }
  if (value->persistence_uncertain)
    return fail(oauth_error::storage_failed);
  if (value->record.account.state == oauth_account_state::reauthentication_required)
    return fail(oauth_error::reauthentication_required);
  const auto now = std::chrono::system_clock::now();
  if (value->record.tokens.expires_at > now &&
      value->record.tokens.expires_at - now > impl_->options.refresh_before_expiry)
    return { value->record.tokens.access_token,
      value->record.tokens.expires_at,
      {},
      value->record.account,
      value->revision };
  const auto provider = impl_->refreshers.find(account.provider);
  if (value->record.tokens.refresh_token.empty() || provider == impl_->refreshers.end())
    return fail(oauth_error::reauthentication_required);
  auto refresher = provider->second;
  const auto previous = value->record;
  value->refreshing = true;
  lock.unlock();
  oauth_refresh_result refreshed;
  try {
    refreshed = refresher->refresh(previous.account, previous.tokens, stop);
  }
  catch (...) {
    refreshed.error = oauth_error::refresh_failed;
  }
  lock.lock();
  value->refreshing = false;
  value->changed.notify_all();
  if (!current())
    return fail(oauth_error::account_changed);
  if (!refreshed.error && (!valid_tokens(refreshed.tokens, impl_->options.max_token_bytes) ||
                            refreshed.tokens.expires_at <= std::chrono::system_clock::now()))
    refreshed.error = oauth_error::invalid_response;
  auto error = refreshed.error;
  if (!error) {
    if (refreshed.tokens.refresh_token.empty())
      refreshed.tokens.refresh_token = previous.tokens.refresh_token;
    if (refreshed.tokens.id_token.empty())
      refreshed.tokens.id_token = previous.tokens.id_token;
    auto updated = previous;
    updated.tokens = std::move(refreshed.tokens);
    // Persist even if cancellation arrived after a successful rotation: dropping
    // the rotated token would strand the account. Caller still sees cancelled.
    error = impl_->persist(key, &updated);
    if (!error)
      value->record = std::move(updated);
    else {
      value->persistence_uncertain = true;
      value->record.account.state = oauth_account_state::reauthentication_required;
    }
  }
  else if (error == oauth_error::reauthentication_required) {
    auto updated = previous;
    updated.account.state = oauth_account_state::reauthentication_required;
    if (auto storage_error = impl_->persist(key, &updated)) {
      error = storage_error;
      value->persistence_uncertain = true;
    }
    value->record.account.state = oauth_account_state::reauthentication_required;
  }
  value->last_error = error;
  if (error)
    return fail(error);
  if (stop.stop_requested())
    return fail(oauth_error::cancelled);
  return { value->record.tokens.access_token,
    value->record.tokens.expires_at,
    {},
    value->record.account,
    value->revision };
}

WUWE_NAMESPACE_END
