#include <wuwe/agent/auth/codex_oauth.h>
#include <wuwe/agent/auth/oauth_credential_store.h>
#include <wuwe/net/http_client.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <functional>
#include <future>
#include <iostream>
#include <latch>
#include <mutex>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;
using namespace wuwe;
using json = nlohmann::json;
namespace {
void require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
codex_oauth_options options() {
  return { .client_id = "test-client", .client_version = "0.153.4" };
}
std::string b64(std::string_view value) {
  constexpr std::string_view alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  unsigned int bits = 0;
  int count = 0;
  std::string result;
  for (unsigned char c : value) {
    bits = (bits << 8) | c;
    count += 8;
    while (count >= 6) {
      count -= 6;
      result += alphabet[(bits >> count) & 63];
    }
  }
  if (count)
    result += alphabet[(bits << (6 - count)) & 63];
  return result;
}
std::string jwt(json claims) {
  return b64(R"({"alg":"RS256"})") + "." + b64(claims.dump()) + ".synthetic";
}
std::int64_t future_expiry() {
  return std::chrono::duration_cast<std::chrono::seconds>(
    (std::chrono::system_clock::now() + 1h).time_since_epoch())
    .count();
}
json id_claims(std::string subject = "user-1", std::string workspace = "workspace-1") {
  return { { "iss", "https://auth.openai.com" },
    { "aud", "test-client" },
    { "exp", future_expiry() },
    { "sub", subject },
    { "email", "test@example.invalid" },
    { "https://api.openai.com/auth", { { "chatgpt_account_id", workspace } } } };
}
json token_body(std::string subject = "user-1", std::string workspace = "workspace-1") {
  return { { "access_token",
             jwt({ { "exp", future_expiry() },
               { "https://api.openai.com/auth", { { "chatgpt_account_id", workspace } } } }) },
    { "refresh_token", "refresh-secret" },
    { "id_token", jwt(id_claims(subject, workspace)) } };
}
http_response response(json body, int status = 200) {
  return { .status_code = status, .body = body.dump() };
}
class fake_http final : public http_client {
  std::mutex mutex_;

public:
  std::deque<std::function<http_response(const http_request&, std::stop_token)>> steps;
  std::atomic<int> calls { 0 };
  void push(json body, int status = 200) {
    steps.push_back(
      [body = std::move(body), status](const auto&, auto) { return response(body, status); });
  }
  http_response send(const http_request&) override {
    throw std::runtime_error("stream expected");
  }
  http_response send_stream(const http_request& request, const http_stream_chunk_callback& chunk,
    std::stop_token stop) override {
    std::function<http_response(const http_request&, std::stop_token)> next;
    {
      std::lock_guard lock(mutex_);
      require(!steps.empty(), "unexpected HTTP request");
      next = std::move(steps.front());
      steps.pop_front();
      ++calls;
    }
    require(!request.follow_redirects && request.timeout > 0 && request.tls.verify_peer &&
              request.tls.verify_host,
      "unsafe HTTP settings");
    auto result = next(request, stop);
    if (!result.transport_error && !result.body.empty() && !chunk(result.body))
      result.error_code = std::make_error_code(std::errc::operation_canceled);
    result.body.clear();
    return result;
  }
};
void enqueue_login(const std::shared_ptr<fake_http>& http, json tokens = token_body()) {
  http->push({ { "device_auth_id", "private-device-id" },
    { "user_code", "ABCD-EFGH" },
    { "interval", "1" } });
  http->push({ { "authorization_code", "code+secret" }, { "code_verifier", "verifier/secret" } });
  http->push(std::move(tokens));
}
oauth_account_info login(codex_oauth_client& client) {
  auto start = client.start_login();
  require(!start.error && start.challenge.has_value(), "start failed");
  require(
    start.challenge->session_id != "private-device-id" && start.challenge->session_id.size() == 64,
    "upstream session leaked");
  require(start.challenge->verification_uri == "https://auth.openai.com/codex/device",
    "wrong verification URI");
  auto poll = client.poll_login(start.challenge->session_id);
  require(!poll.error && poll.state == oauth_login_state::authorized && poll.account.has_value(),
    "login failed");
  return *poll.account;
}
void test_login_catalog_and_refresh() {
  auto http = std::make_shared<fake_http>();
  auto refresh = make_codex_oauth_refresher(options(), http);
  oauth_account_manager manager(make_memory_oauth_credential_store(), { refresh });
  codex_oauth_client client(manager, options(), http);
  enqueue_login(http);
  auto original_start = http->steps[0];
  http->steps[0] = [original_start](const http_request& r, auto stop) {
    require(r.url == "https://auth.openai.com/api/accounts/deviceauth/usercode" &&
              json::parse(r.body)["client_id"] == "test-client",
      "wrong start request");
    return original_start(r, stop);
  };
  auto original_exchange = http->steps[2];
  http->steps[2] = [original_exchange](const http_request& r, auto stop) {
    require(r.url == "https://auth.openai.com/oauth/token" &&
              r.body.find("code=code%2Bsecret") != std::string::npos &&
              r.body.find("code_verifier=verifier%2Fsecret") != std::string::npos &&
              r.body.find("redirect_uri=https%3A%2F%2Fauth.openai.com%2Fdeviceauth%2Fcallback") !=
                std::string::npos,
      "wrong exchange form");
    return original_exchange(r, stop);
  };
  const auto account = login(client);
  require(account.key.provider == "codex_oauth" && account.subject_id == "user-1" &&
            account.upstream_account_id == "workspace-1" &&
            account.key.account_id != account.subject_id,
    "identity binding failed");
  http->steps.push_back([&](const http_request& r, auto) {
    require(r.method == "GET" &&
              r.url == "https://chatgpt.com/backend-api/codex/models?client_version=0.153.4",
      "wrong model endpoint");
    const auto header = [&](std::string_view name, std::string_view value) {
      return std::find(r.headers.begin(),
               r.headers.end(),
               std::pair<std::string, std::string>(name, value)) != r.headers.end();
    };
    require(header("chatgpt-account-id", "workspace-1") && header("originator", "codex_cli_rs") &&
              header("version", "0.153.4"),
      "wrong account/client headers");
    require(r.headers.front().second.starts_with("Bearer "), "missing bearer token");
    return response({ { "models",
      json::array({ { { "slug", "model-a" }, { "display_name", "Model A" } },
        { { "slug", "model-b" } },
        { { "slug", "model-a" } } }) } });
  });
  const auto models = client.list_models(account.key);
  require(!models.error && models.models.size() == 2 && models.models[0].id == "model-a" &&
            models.models[0].display_name == "Model A",
    "model parsing failed");
  auto old_tokens =
    oauth_tokens { "expired-soon", "a+/&", "old-id", std::chrono::system_clock::now() + 1s };
  require(!manager.save_authorization(account, old_tokens), "refresh setup failed");
  const auto revision = manager.account_snapshot(account.key).revision;
  http->steps.push_back([](const http_request& r, auto) {
    require(r.body.find("grant_type=refresh_token") != std::string::npos &&
              r.body.find("refresh_token=a%2B%2F%26") != std::string::npos,
      "refresh encoding failed");
    auto body = token_body();
    body.erase("id_token");
    return response(body);
  });
  const auto access = manager.access_token(account.key);
  require(!access.error && access.account.key == account.key && access.revision == revision &&
            access.expires_at > std::chrono::system_clock::now() + 50min,
    "JWT refresh expiry failed");
}
void test_pending_limits_and_expiry() {
  auto http = std::make_shared<fake_http>();
  oauth_account_manager manager(make_memory_oauth_credential_store(), {});
  auto config = options();
  config.max_login_sessions = 1;
  codex_oauth_client client(manager, config, http);
  http->push(
    { { "device_auth_id", "d" }, { "usercode", "CODE" }, { "interval", 1 }, { "expires_in", 2 } });
  const auto start = client.start_login();
  require(!start.error, "start alias failed");
  require(client.start_login().error == oauth_error::limit_exceeded, "session bound bypassed");
  http->push(json::object(), 403);
  const auto first = client.poll_login(start.challenge->session_id);
  require(!first.error && first.state == oauth_login_state::pending, "403 was not pending");
  require(!client.poll_login(start.challenge->session_id).error && http->calls == 2,
    "poll interval not enforced");
  std::this_thread::sleep_for(1100ms);
  http->push(json::object(), 404);
  require(client.poll_login(start.challenge->session_id).state == oauth_login_state::pending,
    "404 was not pending");
  std::this_thread::sleep_for(1000ms);
  require(client.poll_login(start.challenge->session_id).state == oauth_login_state::expired &&
            http->calls == 3,
    "expiry not enforced");
  http->push({ { "device_auth_id", "d" }, { "user_code", "CODE" }, { "interval", 1 } });
  const auto second = client.start_login();
  require(!second.error, "expired session not pruned");
  http->steps.push_back([](const auto&, auto) {
    auto r = response({ { "error", "slow_down" } }, 429);
    r.headers.push_back({ "Retry-After", "9" });
    return r;
  });
  const auto slow = client.poll_login(second.challenge->session_id);
  require(slow.state == oauth_login_state::slow_down && slow.poll_after >= 9s &&
            slow.error == oauth_error::rate_limited,
    "slow down lost");
  require(!client.cancel_login(second.challenge->session_id) &&
            client.poll_login(second.challenge->session_id).state == oauth_login_state::cancelled,
    "cancel failed");
}
struct failing_store final : oauth_credential_store {
  bool fail { true };
  std::vector<oauth_account_record> records;
  oauth_store_result load() override {
    return { records, {} };
  }
  std::error_code save(const std::vector<oauth_account_record>& value) override {
    if (fail)
      return oauth_error::storage_failed;
    records = value;
    return {};
  }
};
void test_persistence_retry_and_duplicates() {
  auto http = std::make_shared<fake_http>();
  auto store = std::make_unique<failing_store>();
  auto* state = store.get();
  oauth_account_manager manager(std::move(store), {});
  codex_oauth_client client(manager, options(), http);
  enqueue_login(http);
  const auto start = client.start_login();
  auto poll = client.poll_login(start.challenge->session_id);
  require(poll.error == oauth_error::storage_failed && !poll.account && manager.accounts().empty(),
    "failed save reported login success");
  state->fail = false;
  poll = client.poll_login(start.challenge->session_id);
  require(!poll.error && poll.state == oauth_login_state::authorized && http->calls == 3,
    "exchange replayed on save retry");
  require(state->records[0].account.subject_id == "user-1", "subject not stored");
  enqueue_login(http);
  const auto duplicate = client.start_login();
  require(client.poll_login(duplicate.challenge->session_id).error == oauth_error::account_exists &&
            manager.accounts().size() == 1,
    "duplicate identity inserted");
}
void test_targeted_reauthentication() {
  auto http = std::make_shared<fake_http>();
  oauth_account_manager manager(make_memory_oauth_credential_store(), {});
  codex_oauth_client client(manager, options(), http);
  enqueue_login(http);
  const auto account = login(client);
  const auto revision = manager.account_snapshot(account.key).revision;
  enqueue_login(http, token_body("other-user"));
  auto start = client.start_reauthentication(account.key);
  require(client.poll_login(start.challenge->session_id).error == oauth_error::account_changed &&
            manager.account_snapshot(account.key).revision == revision,
    "reauth switched user");
  enqueue_login(http);
  start = client.start_reauthentication(account.key);
  require(!manager.remove_account(account.key), "logout failed");
  require(client.poll_login(start.challenge->session_id).error == oauth_error::account_changed &&
            manager.accounts().empty(),
    "login resurrected removed target");
  enqueue_login(http);
  const auto new_account = login(client);
  enqueue_login(http);
  start = client.start_reauthentication(new_account.key);
  auto access = manager.access_token(new_account.key);
  require(
    !manager.save_authorization(new_account, { access.access_token, "r", "i", access.expires_at }),
    "replacement failed");
  require(client.poll_login(start.challenge->session_id).error == oauth_error::account_changed,
    "stale reauth overwrote new generation");
  enqueue_login(http);
  start = client.start_reauthentication(new_account.key);
  const auto success = client.poll_login(start.challenge->session_id);
  require(
    !success.error && success.account->key == new_account.key, "targeted reauth lost local ID");
}
void test_concurrent_poll_cancel_and_logout() {
  auto http = std::make_shared<fake_http>();
  oauth_account_manager manager(make_memory_oauth_credential_store(), {});
  codex_oauth_client client(manager, options(), http);
  enqueue_login(http);
  const auto start = client.start_login();
  std::latch entered(1), release(1);
  auto exchange = http->steps[1];
  http->steps[1] = [&](const auto& r, auto stop) {
    entered.count_down();
    release.wait();
    return exchange(r, stop);
  };
  auto worker =
    std::async(std::launch::async, [&] { return client.poll_login(start.challenge->session_id); });
  entered.wait();
  const auto second = client.poll_login(start.challenge->session_id);
  require(second.state == oauth_login_state::pending && http->calls == 3,
    "concurrent exchange duplicated");
  require(!client.cancel_login(start.challenge->session_id), "concurrent cancel failed");
  release.count_down();
  require(worker.get().state == oauth_login_state::cancelled && manager.accounts().empty(),
    "late exchange ignored cancel");
  enqueue_login(http);
  const auto account = login(client);
  http->steps.push_back([&](const auto&, auto) {
    require(!manager.remove_account(account.key), "model logout failed");
    return response({ { "models", json::array({ { { "slug", "stale" } } }) } });
  });
  const auto models = client.list_models(account.key);
  require(models.error == oauth_error::account_changed && models.models.empty(),
    "catalog survived logout");
}
void test_invalid_tokens_and_catalogs() {
  auto http = std::make_shared<fake_http>();
  oauth_account_manager manager(make_memory_oauth_credential_store(), {});
  codex_oauth_client client(manager, options(), http);
  std::vector<json> bad;
  auto body = token_body();
  body["access_token"] = "opaque-without-expiry";
  bad.push_back(body);
  body = token_body();
  auto claims = id_claims();
  claims["aud"] = "other-client";
  body["id_token"] = jwt(claims);
  bad.push_back(body);
  body = token_body();
  claims = id_claims();
  claims["iss"] = "https://untrusted.invalid";
  body["id_token"] = jwt(claims);
  bad.push_back(body);
  body = token_body();
  claims = id_claims();
  claims["exp"] = 1;
  body["id_token"] = jwt(claims);
  bad.push_back(body);
  body = token_body();
  body["expires_in"] = 0;
  bad.push_back(body);
  body = token_body();
  body["id_token"] = jwt(id_claims("u", "other-workspace"));
  bad.push_back(body);
  body = token_body();
  body["refresh_token"] = "";
  bad.push_back(body);
  for (const auto& invalid : bad) {
    enqueue_login(http, invalid);
    const auto start = client.start_login();
    require(client.poll_login(start.challenge->session_id).error == oauth_error::invalid_response &&
              manager.accounts().empty(),
      "invalid token accepted");
  }
  enqueue_login(http);
  const auto account = login(client);
  for (const auto& catalog : { json::object(),
         json { { "models", "bad" } },
         json { { "models", json::array({ { { "slug", "valid" } }, { { "slug", 5 } } }) } } }) {
    http->push(catalog);
    const auto result = client.list_models(account.key);
    require(result.error == oauth_error::invalid_response && result.models.empty(),
      "partial catalog accepted");
  }
  http->push({ { "models", json::array() } });
  require(!client.list_models(account.key).error, "empty catalog failed");
  http->push({ { "secret", "must not escape" } }, 401);
  require(client.list_models(account.key).error == oauth_error::reauthentication_required &&
            manager.accounts()[0].state == oauth_account_state::ready,
    "catalog 401 incorrectly revoked refresh token");
  auto limited = options();
  limited.max_models = 1;
  codex_oauth_client limited_client(manager, limited, http);
  http->push({ { "models", json::array({ { { "slug", "a" } }, { { "slug", "b" } } }) } });
  require(limited_client.list_models(account.key).error == oauth_error::limit_exceeded,
    "model limit ignored");
  limited.max_response_bytes = 8;
  codex_oauth_client bounded(manager, limited, http);
  http->push({ { "device_auth_id", "too long" } });
  require(bounded.start_login().error == oauth_error::limit_exceeded, "response limit ignored");
}
void test_refresh_identity_and_errors() {
  auto http = std::make_shared<fake_http>();
  auto refresher = make_codex_oauth_refresher(options(), http);
  oauth_account_manager manager(make_memory_oauth_credential_store(), {});
  codex_oauth_client client(manager, options(), http);
  enqueue_login(http);
  const auto account = login(client);
  oauth_tokens old { "old", "refresh", "id", {} };
  http->push(token_body("different-user"));
  require(refresher->refresh(account, old, {}).error == oauth_error::reauthentication_required,
    "refresh switched user");
  http->push({ { "error", { { "code", "refresh_token_reused" } } } }, 400);
  require(refresher->refresh(account, old, {}).error == oauth_error::reauthentication_required,
    "terminal refresh ignored");
  http->push({ { "error", "invalid_grant" } }, 503);
  require(refresher->refresh(account, old, {}).error == oauth_error::refresh_failed,
    "server error revoked account");
  std::stop_source stopped;
  stopped.request_stop();
  const auto count = http->calls.load();
  require(client.start_login(stopped.get_token()).error == oauth_error::cancelled &&
            client.list_models(account.key, stopped.get_token()).error == oauth_error::cancelled &&
            http->calls == count,
    "pre-cancel dispatched HTTP");
}
void test_denial_exchange_failure_and_deadline() {
  auto http = std::make_shared<fake_http>();
  oauth_account_manager manager(make_memory_oauth_credential_store(), {});
  codex_oauth_client client(manager, options(), http);
  const auto challenge =
    json { { "device_auth_id", "device" }, { "user_code", "CODE" }, { "interval", 1 } };
  for (const auto& [code, expected] : std::vector<std::pair<std::string, oauth_login_state>> {
         { "access_denied", oauth_login_state::denied },
         { "expired_token", oauth_login_state::expired } }) {
    http->push(challenge);
    auto start = client.start_login();
    http->push({ { "error", code } }, 403);
    const auto poll = client.poll_login(start.challenge->session_id);
    require(poll.state == expected && manager.accounts().empty(), "terminal authorization ignored");
  }
  enqueue_login(http);
  http->steps[2] = [](const auto&, auto) {
    return http_response { .transport_error = std::make_error_code(std::errc::connection_reset) };
  };
  auto start = client.start_login();
  auto poll = client.poll_login(start.challenge->session_id);
  require(
    poll.state == oauth_login_state::denied && poll.error, "ambiguous exchange left retryable");
  const auto count = http->calls.load();
  require(client.poll_login(start.challenge->session_id).state == oauth_login_state::denied &&
            http->calls == count,
    "single-use exchange replayed");
  auto config = options();
  config.timeout_ms = 1;
  codex_oauth_client bounded(manager, config, http);
  http->steps.push_back([&](const auto&, auto) {
    std::this_thread::sleep_for(10ms);
    return response(challenge);
  });
  require(bounded.start_login().error == oauth_error::timeout, "deadline ignored");
  http->steps.push_back([](const auto&, auto) -> http_response {
    throw std::runtime_error("secret transport details");
  });
  require(
    client.start_login().error == oauth_error::authorization_failed, "transport exception escaped");
  auto invalid = options();
  invalid.originator = "bad\r\nheader";
  bool threw = false;
  try {
    codex_oauth_client rejected(manager, invalid, http);
  }
  catch (const std::invalid_argument&) {
    threw = true;
  }
  require(threw, "header injection accepted");
}

void test_cancelled_refresh_persists_rotation() {
  auto http = std::make_shared<fake_http>();
  oauth_account_manager manager(make_memory_oauth_credential_store(), {});
  codex_oauth_client client(manager, options(), http);
  enqueue_login(http);
  const auto account = login(client);
  // Cancellation at completion, after all response bytes have arrived, must
  // not discard the newly rotated refresh token.
  class completion_http final : public http_client {
  public:
    std::stop_source& cancellation;
    explicit completion_http(std::stop_source& source) : cancellation(source) {
    }
    http_response send(const http_request&) override {
      return {};
    }
    http_response send_stream(
      const http_request&, const http_stream_chunk_callback& chunk, std::stop_token) override {
      auto body = token_body();
      body["refresh_token"] = "rotated";
      require(chunk(body.dump()), "response unexpectedly stopped");
      cancellation.request_stop();
      return { .status_code = 200 };
    }
  };
  std::stop_source stop;
  auto completing = std::make_shared<completion_http>(stop);
  auto direct = make_codex_oauth_refresher(options(), completing);
  auto store = std::make_unique<failing_store>();
  auto* state = store.get();
  state->fail = false;
  oauth_account_manager rotating(std::move(store), { direct });
  require(!rotating.save_authorization(
            account, { "old", "old-refresh", "old-id", std::chrono::system_clock::now() + 1s }),
    "cancel refresh setup failed");
  const auto result = rotating.access_token(account.key, stop.get_token());
  require(
    result.error == oauth_error::cancelled && state->records[0].tokens.refresh_token == "rotated",
    "rotation discarded on cancellation");
}
} // namespace
int main() {
  try {
    test_login_catalog_and_refresh();
    test_pending_limits_and_expiry();
    test_persistence_retry_and_duplicates();
    test_targeted_reauthentication();
    test_concurrent_poll_cancel_and_logout();
    test_invalid_tokens_and_catalogs();
    test_refresh_identity_and_errors();
    test_denial_exchange_failure_and_deadline();
    test_cancelled_refresh_persists_rotation();
    std::cout << "Codex OAuth tests passed\n";
    return 0;
  }
  catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
