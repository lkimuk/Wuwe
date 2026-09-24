#include <wuwe/agent/auth/oauth_account.h>
#include <wuwe/agent/auth/oauth_credential_store.h>
#include <wuwe/agent/auth/oauth_refresh_client.h>
#include <wuwe/net/http_client.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <latch>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;
namespace {
void require(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}
const wuwe::oauth_account_key key { "test_provider", "local-1" };
wuwe::oauth_account_info account() {
  return { key, "Test account", "workspace-1", wuwe::oauth_account_state::ready, "test-subject" };
}
wuwe::oauth_tokens tokens(std::string access = "old-access", std::chrono::seconds lifetime = 20s) {
  return {
    std::move(access), "old-refresh", "old-id", std::chrono::system_clock::now() + lifetime
  };
}

struct store_state {
  std::vector<wuwe::oauth_account_record> records;
  bool fail_save { false };
  bool throw_save { false };
  bool corrupt { false };
  int saves { 0 };
};
class test_store final : public wuwe::oauth_credential_store {
  std::shared_ptr<store_state> state_;

public:
  explicit test_store(std::shared_ptr<store_state> state) : state_(std::move(state)) {
  }
  wuwe::oauth_store_result load() override {
    if (state_->corrupt)
      return { {}, wuwe::oauth_error::storage_corrupt };
    return { state_->records, {} };
  }
  std::error_code save(const std::vector<wuwe::oauth_account_record>& records) override {
    ++state_->saves;
    if (state_->throw_save)
      throw std::runtime_error("secret diagnostic must not escape");
    if (state_->fail_save)
      return wuwe::oauth_error::storage_failed;
    state_->records = records;
    return {};
  }
};
class refresher final : public wuwe::oauth_token_refresher {
public:
  std::atomic<int> calls { 0 };
  std::function<wuwe::oauth_refresh_result(const wuwe::oauth_tokens&, std::stop_token)> action;
  std::string provider_id() const override {
    return "test_provider";
  }
  wuwe::oauth_refresh_result refresh(const wuwe::oauth_account_info& info,
    const wuwe::oauth_tokens& previous, std::stop_token stop) override {
    require(info.key == key && info.upstream_account_id == "workspace-1", "wrong account binding");
    ++calls;
    if (action)
      return action(previous, stop);
    return { tokens("new-access", 1h), {} };
  }
};

void test_lifecycle_and_storage_failures() {
  auto state = std::make_shared<store_state>();
  auto refresh = std::make_shared<refresher>();
  wuwe::oauth_account_manager manager(std::make_unique<test_store>(state), { refresh });
  require(!manager.save_authorization(account(), tokens("cached", 1h)), "save failed");
  require(
    manager.accounts().size() == 1 && manager.accounts()[0].key == key, "account listing failed");
  require(
    manager.access_token(key).access_token == "cached" && refresh->calls == 0, "cache missed");
  require(
    manager.access_token({ "other", key.account_id }).error == wuwe::oauth_error::account_not_found,
    "provider identity was ignored");
  state->fail_save = true;
  require(manager.remove_account(key) == wuwe::oauth_error::storage_failed &&
            manager.accounts().size() == 1,
    "failed logout changed memory");
  require(manager.save_authorization(account(), tokens("replacement", 1h)) ==
            wuwe::oauth_error::storage_failed,
    "save failure ignored");
  require(manager.access_token(key).access_token == "cached", "failed replacement changed memory");
  state->fail_save = false;
  require(!manager.remove_account(key) && state->records.empty(), "logout not persisted");
  require(manager.access_token(key).error == wuwe::oauth_error::account_not_found,
    "removed account still usable");
  require(manager.save_authorization(account(), tokens("expired", -1s)) ==
            wuwe::oauth_error::invalid_argument,
    "expired authorization accepted");
  state->corrupt = true;
  const auto saves = state->saves;
  try {
    wuwe::oauth_account_manager broken(std::make_unique<test_store>(state), {});
    require(false, "corrupt vault accepted");
  }
  catch (const std::system_error& error) {
    require(error.code() == wuwe::oauth_error::storage_corrupt, "wrong corruption error");
  }
  require(state->saves == saves, "corrupt vault overwritten");
}

void test_refresh_rotation_and_failure() {
  auto state = std::make_shared<store_state>();
  auto refresh = std::make_shared<refresher>();
  wuwe::oauth_account_manager manager(std::make_unique<test_store>(state), { refresh });
  require(!manager.save_authorization(account(), tokens()), "save failed");
  refresh->action = [](const auto& previous, std::stop_token) {
    require(previous.refresh_token == "old-refresh", "wrong refresh credential");
    auto next = tokens("rotated", 1h);
    next.refresh_token = "rotated-refresh";
    next.id_token.clear();
    return wuwe::oauth_refresh_result { next, {} };
  };
  require(manager.access_token(key).access_token == "rotated", "rotation failed");
  require(state->records[0].tokens.refresh_token == "rotated-refresh" &&
            state->records[0].tokens.id_token == "old-id",
    "rotation not durably merged");
  require(!manager.save_authorization(account(), tokens()), "reauthorization failed");
  state->throw_save = true;
  require(manager.access_token(key).error == wuwe::oauth_error::storage_failed,
    "write failure not contained");
  state->throw_save = false;
  const auto count = refresh->calls.load();
  require(
    manager.access_token(key).error == wuwe::oauth_error::storage_failed && refresh->calls == count,
    "uncertain rotated credential reused");
  require(manager.accounts()[0].state == wuwe::oauth_account_state::reauthentication_required,
    "rotation failure not visible");
  require(!manager.save_authorization(account(), tokens()), "recovery failed");
  refresh->action = [](const auto&, std::stop_token) {
    return wuwe::oauth_refresh_result { .error = wuwe::oauth_error::reauthentication_required };
  };
  require(manager.access_token(key).error == wuwe::oauth_error::reauthentication_required,
    "terminal refresh ignored");
  const auto terminal_calls = refresh->calls.load();
  require(manager.access_token(key).error == wuwe::oauth_error::reauthentication_required &&
            refresh->calls == terminal_calls,
    "terminal refresh retried");
  wuwe::oauth_account_manager restarted(std::make_unique<test_store>(state), { refresh });
  require(restarted.access_token(key).error == wuwe::oauth_error::reauthentication_required,
    "terminal state not persisted");
}

void test_expired_persisted_token() {
  auto state = std::make_shared<store_state>();
  auto expired = tokens();
  expired.expires_at = std::chrono::system_clock::time_point::min();
  state->records.push_back({ account(), expired });
  auto refresh = std::make_shared<refresher>();
  wuwe::oauth_account_manager manager(std::make_unique<test_store>(state), { refresh });
  require(manager.access_token(key).access_token == "new-access" && refresh->calls == 1,
    "expired persisted token was returned instead of refreshed");
}

void test_single_flight_and_independent_cancellation() {
  auto refresh = std::make_shared<refresher>();
  std::latch entered(1), release(1);
  refresh->action = [&](const auto&, std::stop_token) {
    entered.count_down();
    release.wait();
    return wuwe::oauth_refresh_result { tokens("shared-result", 1h), {} };
  };
  wuwe::oauth_account_manager manager(wuwe::make_memory_oauth_credential_store(), { refresh });
  require(!manager.save_authorization(account(), tokens()), "save failed");
  auto first = std::async(std::launch::async, [&] { return manager.access_token(key); });
  entered.wait();
  std::stop_source cancelled;
  std::latch waiter_started(1);
  auto waiter = std::async(std::launch::async, [&] {
    waiter_started.count_down();
    return manager.access_token(key, cancelled.get_token());
  });
  waiter_started.wait();
  const bool was_waiting = waiter.wait_for(30ms) == std::future_status::timeout;
  cancelled.request_stop();
  const bool stopped = waiter.wait_for(2s) == std::future_status::ready;
  std::vector<std::future<wuwe::oauth_access_result>> waiters;
  for (int i = 0; i < 12; ++i)
    waiters.push_back(std::async(std::launch::async, [&] { return manager.access_token(key); }));
  release.count_down();
  require(was_waiting && stopped && waiter.get().error == wuwe::oauth_error::cancelled,
    "waiter cancellation blocked");
  require(first.get().access_token == "shared-result", "initiator failed");
  for (auto& result : waiters)
    require(result.get().access_token == "shared-result", "waiter failed");
  require(refresh->calls == 1, "concurrent callers rotated more than once");
}

void test_logout_and_relogin_fence() {
  for (bool replace : { false, true }) {
    auto state = std::make_shared<store_state>();
    auto refresh = std::make_shared<refresher>();
    std::latch entered(1), release(1);
    refresh->action = [&](const auto&, std::stop_token) {
      entered.count_down();
      release.wait();
      return wuwe::oauth_refresh_result { tokens("stale-result", 1h), {} };
    };
    wuwe::oauth_account_manager manager(std::make_unique<test_store>(state), { refresh });
    require(!manager.save_authorization(account(), tokens()), "save failed");
    auto pending = std::async(std::launch::async, [&] { return manager.access_token(key); });
    entered.wait();
    const auto error = replace ? manager.save_authorization(account(), tokens("new-login", 1h))
                               : manager.remove_account(key);
    release.count_down();
    require(!error, "account change failed");
    require(
      pending.get().error == wuwe::oauth_error::account_changed, "old refresh crossed generation");
    if (replace)
      require(manager.access_token(key).access_token == "new-login", "old refresh replaced login");
    else
      require(state->records.empty() && manager.accounts().empty(),
        "refresh resurrected removed account");
  }
}

void test_cancel_after_rotation_and_transient_errors() {
  auto state = std::make_shared<store_state>();
  auto refresh = std::make_shared<refresher>();
  std::stop_source source;
  refresh->action = [&](const auto&, std::stop_token) {
    source.request_stop();
    auto next = tokens("late-success", 1h);
    next.refresh_token = "late-rotation";
    return wuwe::oauth_refresh_result { next, {} };
  };
  wuwe::oauth_account_manager manager(std::make_unique<test_store>(state), { refresh });
  require(!manager.save_authorization(account(), tokens()), "save failed");
  require(manager.access_token(key, source.get_token()).error == wuwe::oauth_error::cancelled,
    "late cancellation lost");
  require(state->records[0].tokens.refresh_token == "late-rotation" &&
            manager.access_token(key).access_token == "late-success",
    "late cancellation discarded rotated credential");
  require(!manager.save_authorization(account(), tokens()), "save failed");
  refresh->action = [](const auto&, std::stop_token) -> wuwe::oauth_refresh_result {
    throw std::runtime_error("secret");
  };
  require(manager.access_token(key).error == wuwe::oauth_error::refresh_failed,
    "adapter exception leaked");
  require(manager.accounts()[0].state == wuwe::oauth_account_state::ready,
    "transient error invalidated account");
  refresh->action = {};
  require(
    manager.access_token(key).access_token == "new-access", "retry after transient failure failed");
}

class capture_http final : public wuwe::http_client {
public:
  wuwe::http_response response {
    .status_code = 200,
    .body =
      R"({"access_token":"new","refresh_token":"rotated","expires_in":3600,"token_type":"Bearer"})"
  };
  std::vector<wuwe::http_request> requests;
  wuwe::http_response send(const wuwe::http_request&) override {
    throw std::runtime_error("stream required");
  }
  wuwe::http_response send_stream(const wuwe::http_request& request,
    const wuwe::http_stream_chunk_callback& callback, std::stop_token) override {
    requests.push_back(request);
    if (!callback(response.body))
      return { .error_code = std::make_error_code(std::errc::operation_canceled) };
    return response;
  }
};

void test_refresh_http_contract() {
  auto http = std::make_shared<capture_http>();
  wuwe::oauth_refresh_endpoint endpoint {
    "test_provider", "https://auth.example/oauth/token", "client-id"
  };
  wuwe::oauth_refresh_client client(endpoint, http);
  auto previous = tokens();
  previous.refresh_token = "secret+&=/";
  auto result = client.refresh(account(), previous, {});
  require(!result.error && result.tokens.refresh_token == "rotated", "refresh response failed");
  const auto& request = http->requests[0];
  require(request.method == "POST" && !request.follow_redirects && request.timeout == 15000 &&
            request.body ==
              "grant_type=refresh_token&client_id=client-id&refresh_token=secret%2B%26%3D%2F",
    "refresh grant request wrong");
  for (const auto* body : { "{}",
         R"({"access_token":"x","expires_in":0})",
         R"({"access_token":"x","expires_in":9999999999999999999})",
         R"({"access_token":"x","expires_in":3600,"refresh_token":null})",
         R"({"access_token":"x\n","expires_in":3600})" }) {
    http->response = { .status_code = 200, .body = body };
    require(client.refresh(account(), previous, {}).error == wuwe::oauth_error::invalid_response,
      "bad response accepted");
  }
  http->response = { .status_code = 400,
    .body = R"({"error":"invalid_grant","error_description":"secret+&=/"})" };
  require(
    client.refresh(account(), previous, {}).error == wuwe::oauth_error::reauthentication_required,
    "invalid grant not terminal");
  http->response.error_code = std::make_error_code(std::errc::connection_reset);
  require(client.refresh(account(), previous, {}).error == wuwe::oauth_error::refresh_failed,
    "transport failure misclassified as revoked credentials");
  http->response = { .status_code = 401,
    .body = R"({"error":{"code":"REFRESH_TOKEN_INVALIDATED"}})" };
  require(
    client.refresh(account(), previous, {}).error == wuwe::oauth_error::reauthentication_required,
    "nested provider revocation code was ignored");
  http->response = { .status_code = 200,
    .body = R"({"access_token":"new","expires_in":3600,"token_type":"bEaReR"})" };
  require(!client.refresh(account(), previous, {}).error, "token_type is case insensitive");
  for (int status : { 302, 401, 429, 500 }) {
    http->response = { .status_code = status, .body = "upstream echoed secret+&=/" };
    const auto failed = client.refresh(account(), previous, {});
    require(failed.error == wuwe::oauth_error::refresh_failed &&
              failed.tokens.access_token.empty() &&
              failed.error.message().find("secret") == std::string::npos,
      "failure leaked body or guessed terminal auth");
  }
  endpoint.max_response_bytes = 3;
  wuwe::oauth_refresh_client bounded(endpoint, http);
  require(bounded.refresh(account(), previous, {}).error == wuwe::oauth_error::invalid_response,
    "body limit ignored");
  auto wrong = account();
  wrong.key.provider = "other";
  const auto count = http->requests.size();
  require(client.refresh(wrong, previous, {}).error == wuwe::oauth_error::invalid_argument &&
            http->requests.size() == count,
    "credentials sent to wrong provider");
}

void test_secure_store() {
#ifdef _WIN32
  const auto dir = std::filesystem::current_path() /
                   ("oauth-vault-test-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  require(std::filesystem::create_directory(dir), "cannot create test directory");
  struct cleanup {
    std::filesystem::path path;
    ~cleanup() {
      std::error_code ignored;
      std::filesystem::remove_all(path, ignored);
    }
  } cleanup_dir { dir };
  const auto path = dir / "credentials.bin";
  {
    auto store = wuwe::make_system_oauth_credential_store(path);
    require(store->load().records.empty(), "new vault not empty");
    try {
      auto duplicate = wuwe::make_system_oauth_credential_store(path);
      require(false, "duplicate vault owner allowed");
    }
    catch (const std::system_error& error) {
      require(error.code() == wuwe::oauth_error::storage_in_use, "wrong ownership error");
    }
    wuwe::oauth_account_manager manager(std::move(store), {});
    require(!manager.save_authorization(account(), tokens("sensitive-access-unique", 1h)),
      "encrypted save failed");
    std::ifstream file(path, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(file)), {});
    require(bytes.find("sensitive-access-unique") == std::string::npos &&
              bytes.find("old-refresh") == std::string::npos,
      "plaintext credentials on disk");
  }
  {
    wuwe::oauth_account_manager manager(wuwe::make_system_oauth_credential_store(path), {});
    require(
      manager.access_token(key).access_token == "sensitive-access-unique", "vault reopen failed");
    require(manager.account_snapshot(key).account->subject_id == "test-subject",
      "vault lost subject identity");
    // Force replacement failure without destroying the existing vault.
    std::filesystem::create_directory(path.wstring() + L".pending");
    require(manager.remove_account(key) == wuwe::oauth_error::storage_failed,
      "replacement failure ignored");
    require(manager.access_token(key).access_token == "sensitive-access-unique",
      "failed write changed account");
    std::filesystem::remove(path.wstring() + L".pending");
    require(!manager.remove_account(key), "encrypted logout failed");
  }
  {
    auto store = wuwe::make_system_oauth_credential_store(path);
    require(store->load().records.empty(), "logout not durable");
  }
  {
    std::fstream corrupt(path, std::ios::binary | std::ios::in | std::ios::out);
    corrupt << "broken vault";
    corrupt.flush();
    require(corrupt.good(), "failed to corrupt test vault");
  }
  {
    auto store = wuwe::make_system_oauth_credential_store(path);
    require(store->load().error == wuwe::oauth_error::storage_corrupt,
      "corrupt encrypted vault accepted");
  }
#else
  try {
    auto unsupported = wuwe::make_system_oauth_credential_store("/unused");
    require(false, "unexpected plaintext fallback");
  }
  catch (const std::system_error& error) {
    require(error.code() == wuwe::oauth_error::storage_unavailable, "wrong unsupported error");
  }
#endif
}
} // namespace

int main() {
  try {
    test_lifecycle_and_storage_failures();
    test_refresh_rotation_and_failure();
    test_expired_persisted_token();
    test_single_flight_and_independent_cancellation();
    test_logout_and_relogin_fence();
    test_cancel_after_rotation_and_transient_errors();
    test_refresh_http_contract();
    test_secure_store();
    std::cout << "OAuth account tests passed\n";
    return 0;
  }
  catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
