#include "codex_oauth_protocol.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <set>

WUWE_NAMESPACE_BEGIN
using namespace codex_detail;
using namespace std::chrono_literals;

class codex_oauth_client::impl {
public:
  struct session {
    std::string id;
    std::string device_id;
    std::string user_code;
    std::chrono::steady_clock::time_point expires;
    std::chrono::steady_clock::time_point next_poll {};
    std::chrono::seconds interval { 5 };
    oauth_login_state state { oauth_login_state::pending };
    bool busy { true };
    std::stop_source cancellation;
    oauth_account_snapshot target;
    std::optional<oauth_account_record> authorized;
    std::optional<oauth_account_info> saved;
    std::error_code terminal_error;
  };
  oauth_account_manager& accounts;
  codex_oauth_options options;
  std::shared_ptr<http_client> http;
  std::mutex mutex;
  std::map<std::string, std::shared_ptr<session>, std::less<>> sessions;

  impl(oauth_account_manager& a, codex_oauth_options o, std::shared_ptr<http_client> h)
      : accounts(a), options(std::move(o)), http(transport(std::move(h))) {
    validate(options);
  }
  static bool terminal(const session& s) {
    return s.state != oauth_login_state::pending && s.state != oauth_login_state::slow_down;
  }
  static void finish(session& s, oauth_login_state state, std::error_code error = {}) {
    s.state = state;
    s.terminal_error = error;
    s.device_id.clear();
    s.user_code.clear();
    s.authorized.reset();
  }
  static oauth_login_poll_result result(const session& s, std::error_code error = {}) {
    auto after = s.interval;
    if (s.next_poll > std::chrono::steady_clock::now())
      after =
        std::chrono::ceil<std::chrono::seconds>(s.next_poll - std::chrono::steady_clock::now());
    return {
      s.state, s.saved, terminal(s) ? 0s : std::max(1s, after), error ? error : s.terminal_error
    };
  }
  oauth_login_start_result start(oauth_account_snapshot target, std::stop_token stop) {
    if (stop.stop_requested())
      return { .error = oauth_error::cancelled };
    auto s = std::make_shared<session>();
    try {
      s->id = random_id();
    }
    catch (...) {
      return { .error = oauth_error::authorization_failed };
    }
    s->target = std::move(target);
    // Reserve capacity before network dispatch, including concurrent starts.
    const auto started = std::chrono::steady_clock::now();
    s->expires = started + 15min;
    {
      std::lock_guard lock(mutex);
      std::erase_if(sessions, [&](const auto& entry) {
        return !entry.second->busy && (terminal(*entry.second) || entry.second->expires <= started);
      });
      if (sessions.size() >= options.max_login_sessions)
        return { .error = oauth_error::limit_exceeded };
      if (!sessions.emplace(s->id, s).second)
        return { .error = oauth_error::authorization_failed };
    }
    auto response = request(*http,
      options,
      json_post("https://auth.openai.com/api/accounts/deviceauth/usercode",
        { { "client_id", options.client_id } }),
      stop);
    std::lock_guard lock(mutex);
    const auto fail = [&](std::error_code error) {
      sessions.erase(s->id);
      return oauth_login_start_result { .error = error };
    };
    if (stop.stop_requested())
      return fail(oauth_error::cancelled);
    if (response.error)
      return fail(response.error);
    if (!response.success())
      return fail(
        response.status == 429 ? oauth_error::rate_limited : oauth_error::authorization_failed);
    if (!response.body.is_object())
      return fail(oauth_error::invalid_response);
    s->device_id = string_field(response.body, "device_auth_id");
    s->user_code = string_field(response.body, "user_code");
    if (s->user_code.empty())
      s->user_code = string_field(response.body, "usercode");
    if (!safe_text(s->device_id, 4096) || !safe_text(s->user_code, 256))
      return fail(oauth_error::invalid_response);
    auto interval = response.body.find("interval");
    if (interval != response.body.end()) {
      auto parsed = seconds_field(*interval, 900);
      if (!parsed)
        return fail(oauth_error::invalid_response);
      s->interval = *parsed;
    }
    auto duration = 900s;
    if (response.body.contains("expires_in")) {
      auto parsed = seconds_field(response.body["expires_in"], 24 * 60 * 60);
      if (!parsed)
        return fail(oauth_error::invalid_response);
      duration = std::min(duration, *parsed);
    }
    s->expires = started + duration;
    if (s->expires <= std::chrono::steady_clock::now())
      return fail(oauth_error::timeout);
    s->busy = false;
    return { oauth_device_challenge { s->id,
               "https://auth.openai.com/codex/device",
               s->user_code,
               std::chrono::system_clock::now() +
                 std::chrono::duration_cast<std::chrono::system_clock::duration>(
                   s->expires - std::chrono::steady_clock::now()),
               s->interval },
      {} };
  }
  oauth_login_poll_result persist(session& s) {
    auto& record = *s.authorized;
    const auto error =
      accounts.save_authorization(record.account, record.tokens, s.target.revision);
    if (!error) {
      s.saved = record.account;
      finish(s, oauth_login_state::authorized);
    }
    else if (error != oauth_error::storage_failed && error != oauth_error::storage_unavailable &&
             error != oauth_error::storage_in_use) {
      finish(s, oauth_login_state::denied, error);
    }
    // Retain completed exchange on a recoverable storage failure. A subsequent
    // poll retries only persistence, never replays the single-use code.
    return result(s, error);
  }
};

codex_oauth_client::codex_oauth_client(
  oauth_account_manager& accounts, codex_oauth_options options, std::shared_ptr<http_client> http)
    : impl_(std::make_unique<impl>(accounts, std::move(options), std::move(http))) {
}
codex_oauth_client::~codex_oauth_client() = default;
std::string codex_oauth_client::provider_id() const {
  return provider;
}
oauth_login_start_result codex_oauth_client::start_login(std::stop_token stop) {
  return impl_->start({}, stop);
}
oauth_login_start_result codex_oauth_client::start_reauthentication(
  const oauth_account_key& account, std::stop_token stop) {
  if (account.provider != provider)
    return { .error = oauth_error::invalid_argument };
  auto target = impl_->accounts.account_snapshot(account);
  if (target.error)
    return { .error = target.error };
  if (!safe_text(target.account->subject_id, 1024) ||
      !safe_text(target.account->upstream_account_id, 1024))
    return { .error = oauth_error::invalid_argument };
  return impl_->start(std::move(target), stop);
}
std::error_code codex_oauth_client::cancel_login(std::string_view id) {
  std::shared_ptr<impl::session> s;
  {
    std::lock_guard lock(impl_->mutex);
    const auto at = impl_->sessions.find(id);
    if (at == impl_->sessions.end())
      return oauth_error::invalid_argument;
    s = at->second;
    if (impl::terminal(*s))
      return {};
    impl::finish(*s, oauth_login_state::cancelled, oauth_error::cancelled);
  }
  // stop callbacks may run arbitrary transport code; never invoke under mutex.
  s->cancellation.request_stop();
  return {};
}

oauth_login_poll_result codex_oauth_client::poll_login(std::string_view id, std::stop_token stop) {
  const auto fail = [](
                      std::error_code error) { return oauth_login_poll_result { .error = error }; };
  if (stop.stop_requested())
    return fail(oauth_error::cancelled);
  std::shared_ptr<impl::session> s;
  std::string device_id, user_code;
  {
    std::lock_guard lock(impl_->mutex);
    const auto at = impl_->sessions.find(id);
    if (at == impl_->sessions.end())
      return fail(oauth_error::invalid_argument);
    s = at->second;
    if (impl::terminal(*s))
      return impl::result(*s);
    const auto now = std::chrono::steady_clock::now();
    if (s->expires <= now) {
      impl::finish(*s, oauth_login_state::expired);
      return impl::result(*s);
    }
    if (s->busy)
      return impl::result(*s);
    if (s->authorized)
      return impl_->persist(*s);
    if (now < s->next_poll)
      return impl::result(*s);
    s->busy = true;
    s->next_poll = now + s->interval;
    device_id = s->device_id;
    user_code = s->user_code;
  }
  std::stop_source combined;
  std::stop_callback caller_stop(stop, [&] { combined.request_stop(); });
  std::stop_callback session_stop(s->cancellation.get_token(), [&] { combined.request_stop(); });
  auto response = request(*impl_->http,
    impl_->options,
    json_post("https://auth.openai.com/api/accounts/deviceauth/token",
      { { "device_auth_id", device_id }, { "user_code", user_code } }),
    combined.get_token());
  std::unique_lock lock(impl_->mutex);
  s->busy = false;
  s->next_poll = std::chrono::steady_clock::now() + s->interval;
  const auto interrupted = [&]() {
    if (impl::terminal(*s))
      return true;
    if (s->expires <= std::chrono::steady_clock::now()) {
      impl::finish(*s, oauth_login_state::expired);
      return true;
    }
    return false;
  };
  if (interrupted())
    return impl::result(*s);
  if (combined.stop_requested()) {
    impl::finish(*s, oauth_login_state::cancelled, oauth_error::cancelled);
    return impl::result(*s);
  }
  if (response.error)
    return impl::result(*s, response.error);
  const auto name = error_name(response.body);
  if (name == "access_denied" || name == "authorization_declined") {
    impl::finish(*s, oauth_login_state::denied);
    return impl::result(*s);
  }
  if (name == "expired_token" || name == "expired_device_code") {
    impl::finish(*s, oauth_login_state::expired);
    return impl::result(*s);
  }
  if (name == "slow_down" || response.status == 429) {
    s->state = oauth_login_state::slow_down;
    s->interval = std::min(900s, std::max(s->interval + 5s, response.retry_after));
    s->next_poll = std::chrono::steady_clock::now() + s->interval;
    return impl::result(
      *s, response.status == 429 ? make_error_code(oauth_error::rate_limited) : std::error_code {});
  }
  // The Codex endpoint uses 403/404 while the user has not yet approved.
  if (response.status == 403 || response.status == 404 || name == "authorization_pending") {
    s->state = oauth_login_state::pending;
    return impl::result(*s);
  }
  if (!response.success()) {
    if (response.status < 500)
      impl::finish(*s, oauth_login_state::denied, oauth_error::authorization_failed);
    return impl::result(*s, oauth_error::authorization_failed);
  }
  const auto code = string_field(response.body, "authorization_code");
  const auto verifier = string_field(response.body, "code_verifier");
  if (!safe_text(code, 16 * 1024) || !safe_text(verifier, 4096)) {
    impl::finish(*s, oauth_login_state::denied, oauth_error::invalid_response);
    return impl::result(*s);
  }
  s->busy = true;
  lock.unlock();
  const auto started = std::chrono::system_clock::now();
  response = request(*impl_->http,
    impl_->options,
    token_post(impl_->options,
      "grant_type=authorization_code&redirect_uri=" + encode(callback_url) +
        "&code=" + encode(code) + "&code_verifier=" + encode(verifier)),
    combined.get_token());
  auto parsed =
    response.success() ? decode_tokens(response.body, impl_->options, started) : token_result {};
  lock.lock();
  s->busy = false;
  if (interrupted())
    return impl::result(*s);
  if (combined.stop_requested()) {
    impl::finish(*s, oauth_login_state::cancelled, oauth_error::cancelled);
    return impl::result(*s);
  }
  // A failed exchange has an ambiguous single-use-code outcome; restart login,
  // rather than silently replaying a code the server may already have consumed.
  if (!response.success() || parsed.error) {
    auto error = response.error ? response.error
                 : parsed.error ? parsed.error
                                : make_error_code(oauth_error::authorization_failed);
    impl::finish(*s, oauth_login_state::denied, error);
    return impl::result(*s);
  }
  if (s->target.account) {
    if (s->target.account->subject_id != parsed.account.subject_id ||
        s->target.account->upstream_account_id != parsed.account.upstream_account_id) {
      impl::finish(*s, oauth_login_state::denied, oauth_error::account_changed);
      return impl::result(*s);
    }
    parsed.account.key = s->target.account->key;
  }
  else {
    try {
      parsed.account.key.account_id = random_id();
    }
    catch (...) {
      impl::finish(*s, oauth_login_state::denied, oauth_error::authorization_failed);
      return impl::result(*s);
    }
  }
  s->authorized = oauth_account_record { std::move(parsed.account), std::move(parsed.tokens) };
  return impl_->persist(*s);
}

codex_model_list_result codex_oauth_client::list_models(
  const oauth_account_key& account, std::stop_token stop) {
  const auto fail = [](std::error_code error, int status = 0) {
    return codex_model_list_result { .error = error, .http_status = status };
  };
  if (account.provider != provider)
    return fail(oauth_error::invalid_argument);
  const auto access = impl_->accounts.access_token(account, stop);
  if (access.error)
    return fail(access.error);
  if (!safe_text(access.account.upstream_account_id, 1024))
    return fail(oauth_error::invalid_argument);
  auto response = request(*impl_->http,
    impl_->options,
    { .method = "GET",
      .url = "https://chatgpt.com/backend-api/codex/models?client_version=" +
             encode(impl_->options.client_version),
      .headers = account_headers(
        access, impl_->options.originator, impl_->options.client_version, "application/json") },
    stop);
  if (stop.stop_requested())
    return fail(oauth_error::cancelled, response.status);
  const auto current = impl_->accounts.account_snapshot(account);
  if (current.error || current.revision != access.revision)
    return fail(oauth_error::account_changed, response.status);
  if (response.error)
    return fail(response.error, response.status);
  if (!response.success()) {
    auto error = response.status == 401   ? oauth_error::reauthentication_required
                 : response.status == 429 ? oauth_error::rate_limited
                                          : oauth_error::authorization_failed;
    return fail(error, response.status);
  }
  // Use the official Codex ModelsResponse envelope. Malformed or truncated
  // catalogs are errors, never silently accepted as empty/partial results.
  const auto models = response.body.find("models");
  if (!response.body.is_object() || models == response.body.end() || !models->is_array())
    return fail(oauth_error::invalid_response, response.status);
  if (models->size() > impl_->options.max_models)
    return fail(oauth_error::limit_exceeded, response.status);
  codex_model_list_result result { .http_status = response.status };
  std::set<std::string> seen;
  for (const auto& item : *models) {
    auto slug = string_field(item, "slug");
    auto display = string_field(item, "display_name");
    if (!item.is_object() || !safe_text(slug, 1024) || !safe_text(display, 1024, false) ||
        (item.contains("display_name") && !item["display_name"].is_string()))
      return fail(oauth_error::invalid_response, response.status);
    if (seen.insert(slug).second)
      result.models.push_back({ std::move(slug),
        display.empty() ? std::nullopt : std::optional<std::string>(std::move(display)) });
  }
  return result;
}
WUWE_NAMESPACE_END
