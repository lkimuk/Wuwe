#include "codex_oauth_protocol.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <stdexcept>
#include <wuwe/net/default_http_client.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off
#include <windows.h>
#include <bcrypt.h>
// clang-format on
#endif

WUWE_NAMESPACE_BEGIN
namespace codex_detail {
bool safe_text(std::string_view text, std::size_t limit, bool required) {
  return (!required || !text.empty()) && text.size() <= limit &&
         std::none_of(text.begin(), text.end(), [](unsigned char c) { return c < 32 || c == 127; });
}
std::string string_field(const json& object, const char* key) {
  const auto at = object.find(key);
  return at != object.end() && at->is_string() ? at->get<std::string>() : std::string {};
}
std::string encode(std::string_view value) {
  constexpr char hex[] = "0123456789ABCDEF";
  std::string result;
  for (unsigned char c : value) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
        c == '_' || c == '.' || c == '~')
      result += static_cast<char>(c);
    else {
      result += '%';
      result += hex[c >> 4];
      result += hex[c & 15];
    }
  }
  return result;
}
std::string random_id() {
  std::array<unsigned char, 32> bytes {};
#ifdef _WIN32
  if (BCryptGenRandom(
        nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) <
      0)
    throw std::runtime_error("OAuth random source unavailable");
#else
  std::ifstream source("/dev/urandom", std::ios::binary);
  if (!source.read(
        reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
    throw std::runtime_error("OAuth random source unavailable");
#endif
  constexpr char hex[] = "0123456789abcdef";
  std::string result;
  for (const auto c : bytes) {
    result += hex[c >> 4];
    result += hex[c & 15];
  }
  return result;
}
void validate(const codex_oauth_options& o) {
  if (!safe_text(o.client_id, 256) || !safe_text(o.client_version, 128) ||
      !safe_text(o.originator, 128) || o.timeout_ms <= 0 || o.timeout_ms > 300'000 ||
      o.max_response_bytes == 0 || o.max_response_bytes > 16 * 1024 * 1024 || o.max_models == 0 ||
      o.max_models > 100'000 || o.max_login_sessions == 0 || o.max_login_sessions > 256)
    throw std::invalid_argument("Invalid Codex OAuth options");
}
std::shared_ptr<http_client> transport(std::shared_ptr<http_client> http) {
  return http ? std::move(http) : std::make_shared<default_http_client>();
}
json parse(std::string_view body) {
  try {
    return json::parse(
      body,
      [](int depth, json::parse_event_t, json&) {
        if (depth > 64)
          throw std::runtime_error("JSON nesting limit");
        return true;
      },
      false);
  }
  catch (...) {
    return json(json::value_t::discarded);
  }
}
std::string error_name(const json& body) {
  if (!body.is_object())
    return {};
  const auto at = body.find("error");
  auto result =
    at != body.end() && at->is_object() ? string_field(*at, "code") : string_field(body, "error");
  if (result.empty())
    result = string_field(body, "code");
  for (auto& c : result)
    if (c >= 'A' && c <= 'Z')
      c = static_cast<char>(c - 'A' + 'a');
  return result;
}
std::optional<std::chrono::seconds> seconds_field(const json& value, std::int64_t maximum) {
  std::int64_t number = 0;
  if (value.is_number_unsigned()) {
    const auto n = value.get<std::uint64_t>();
    if (n > static_cast<std::uint64_t>(maximum))
      return {};
    number = static_cast<std::int64_t>(n);
  }
  else if (value.is_number_integer())
    number = value.get<std::int64_t>();
  else if (value.is_string()) {
    const auto& text = value.get_ref<const std::string&>();
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), number);
    if (error != std::errc {} || end != text.data() + text.size())
      return {};
  }
  else
    return {};
  if (number <= 0 || number > maximum)
    return {};
  return std::chrono::seconds(number);
}
reply request(
  http_client& http, const codex_oauth_options& options, http_request req, std::stop_token stop) {
  if (stop.stop_requested())
    return { .error = oauth_error::cancelled };
  req.timeout = options.timeout_ms;
  req.follow_redirects = false;
  const auto deadline =
    std::chrono::steady_clock::now() + std::chrono::milliseconds(options.timeout_ms);
  std::string body;
  bool large = false, timed_out = false;
  http_response response;
  try {
    response = http.send_stream(
      req,
      [&](std::string_view chunk) {
        if (stop.stop_requested())
          return false;
        if (std::chrono::steady_clock::now() >= deadline) {
          timed_out = true;
          return false;
        }
        if (chunk.size() > options.max_response_bytes - body.size()) {
          large = true;
          return false;
        }
        body.append(chunk);
        return true;
      },
      stop);
  }
  catch (...) {
    return { .error =
               stop.stop_requested() ? oauth_error::cancelled : oauth_error::authorization_failed };
  }
  reply result { .status = response.status_code };
  if (large) {
    result.error = oauth_error::limit_exceeded;
    return result;
  }
  if (timed_out || std::chrono::steady_clock::now() >= deadline) {
    result.error = oauth_error::timeout;
    return result;
  }
  if (result.status < 100 || result.status > 599) {
    result.error =
      stop.stop_requested() ? oauth_error::cancelled : oauth_error::authorization_failed;
    return result;
  }
  if (response.transport_error ||
      (response.error_code && response.error_code.category() != http_status_category())) {
    result.error =
      stop.stop_requested() ? oauth_error::cancelled : oauth_error::authorization_failed;
    return result;
  }
  // A complete successful token rotation is still delivered when cancellation
  // arrives at completion; the account manager must persist it before returning.
  if (body.empty() && !response.body.empty()) {
    if (response.body.size() > options.max_response_bytes) {
      result.error = oauth_error::limit_exceeded;
      return result;
    }
    body = std::move(response.body);
  }
  result.body = parse(body);
  if (result.status >= 200 && result.status < 300 && result.body.is_discarded())
    result.error = oauth_error::invalid_response;
  if (const auto retry = find_http_header(response.headers, "Retry-After")) {
    result.retry_after = seconds_field(std::string(*retry), 900).value_or(std::chrono::seconds(0));
  }
  return result;
}
http_request json_post(std::string url, json body) {
  return { .method = "POST",
    .url = std::move(url),
    .headers = { { "Content-Type", "application/json" }, { "Accept", "application/json" } },
    .body = body.dump() };
}
http_request token_post(const codex_oauth_options& options, std::string form) {
  return { .method = "POST",
    .url = token_url,
    .headers = { { "Content-Type", "application/x-www-form-urlencoded" },
      { "Accept", "application/json" } },
    .body = std::move(form) + "&client_id=" + encode(options.client_id) };
}
std::vector<std::pair<std::string, std::string>> account_headers(const oauth_access_result& access,
  const std::string& originator, const std::string& version, std::string accept) {
  return { { "Authorization", "Bearer " + access.access_token },
    { "chatgpt-account-id", access.account.upstream_account_id },
    { "originator", originator },
    { "version", version },
    { "Accept", std::move(accept) } };
}
namespace {
json claims(std::string_view token) {
  const auto first = token.find('.');
  const auto last = token.find('.', first == std::string_view::npos ? 0 : first + 1);
  if (first == std::string_view::npos || first == 0 || last == std::string_view::npos ||
      last == first + 1 || last + 1 == token.size() ||
      token.find('.', last + 1) != std::string_view::npos)
    return {};
  auto payload = token.substr(first + 1, last - first - 1);
  if (payload.size() % 4 == 1)
    return {};
  std::string decoded;
  unsigned int bits = 0;
  int count = 0;
  constexpr std::string_view alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  for (const char c : payload) {
    const auto value = alphabet.find(c);
    if (value == std::string_view::npos)
      return {};
    bits = (bits << 6) | static_cast<unsigned int>(value);
    count += 6;
    if (count >= 8) {
      count -= 8;
      decoded += static_cast<char>((bits >> count) & 255);
    }
  }
  if (count != 0 && (bits & ((1u << count) - 1)) != 0)
    return {};
  return parse(decoded);
}
std::optional<std::chrono::system_clock::time_point> expiry(const json& body) {
  auto at = body.find("exp");
  if (at == body.end() || !at->is_number_integer())
    return {};
  // Bounds conversion to system_clock duration on both Windows and nanosecond clocks.
  const auto seconds = seconds_field(*at, 4'102'444'800LL); // 2100-01-01
  if (!seconds)
    return {};
  return std::chrono::system_clock::time_point(*seconds);
}
bool audience(const json& claims_value, const std::string& client) {
  const auto at = claims_value.find("aud");
  if (at == claims_value.end())
    return false;
  if (at->is_string())
    return *at == client;
  return at->is_array() && std::any_of(at->begin(), at->end(), [&](const json& item) {
    return item.is_string() && item == client;
  });
}
} // namespace
token_result decode_tokens(const json& body, const codex_oauth_options& options,
  std::chrono::system_clock::time_point started, const oauth_account_info* previous) {
  const auto fail = [](oauth_error code) { return token_result { .error = code }; };
  if (!body.is_object())
    return fail(oauth_error::invalid_response);
  token_result result;
  auto& tokens = result.tokens;
  tokens.access_token = string_field(body, "access_token");
  tokens.refresh_token = string_field(body, "refresh_token");
  tokens.id_token = string_field(body, "id_token");
  if (!safe_text(tokens.access_token, 256 * 1024) ||
      !safe_text(tokens.refresh_token, 256 * 1024, !previous || body.contains("refresh_token")) ||
      !safe_text(tokens.id_token, 256 * 1024, !previous || body.contains("id_token")))
    return fail(oauth_error::invalid_response);
  if (body.contains("token_type")) {
    auto type = string_field(body, "token_type");
    for (auto& c : type)
      if (c >= 'A' && c <= 'Z')
        c = static_cast<char>(c - 'A' + 'a');
    if (type != "bearer")
      return fail(oauth_error::invalid_response);
  }
  const auto access = claims(tokens.access_token);
  const auto access_expiry = expiry(access);
  if (body.contains("expires_in")) {
    const auto lifetime = seconds_field(body["expires_in"], 31 * 24 * 60 * 60);
    if (!lifetime)
      return fail(oauth_error::invalid_response);
    tokens.expires_at = started + *lifetime;
    if (access_expiry)
      tokens.expires_at = std::min(tokens.expires_at, *access_expiry);
  }
  else {
    if (!access_expiry)
      return fail(oauth_error::invalid_response);
    tokens.expires_at = *access_expiry;
  }
  if (tokens.expires_at <= std::chrono::system_clock::now())
    return fail(oauth_error::invalid_response);
  if (previous)
    result.account = *previous;
  if (!tokens.id_token.empty()) {
    const auto id = claims(tokens.id_token);
    const auto id_expiry = expiry(id);
    if (!id.is_object() || string_field(id, "iss") != issuer || !audience(id, options.client_id) ||
        !id_expiry || *id_expiry <= std::chrono::system_clock::now())
      return fail(oauth_error::invalid_response);
    const auto auth = id.find(auth_claim);
    if (auth == id.end() || !auth->is_object())
      return fail(oauth_error::invalid_response);
    // Enterprise/FedRAMP routing is outside this adapter's fixed endpoint scope.
    const auto fedramp = auth->find("chatgpt_account_is_fedramp");
    if (fedramp != auth->end() && (!fedramp->is_boolean() || fedramp->get<bool>()))
      return fail(oauth_error::authorization_failed);
    result.account.subject_id = string_field(id, "sub");
    result.account.upstream_account_id = string_field(*auth, "chatgpt_account_id");
    result.account.display_name = string_field(id, "email");
    if (result.account.display_name.empty())
      result.account.display_name = "Codex account";
  }
  if (!safe_text(result.account.subject_id, 1024) ||
      !safe_text(result.account.upstream_account_id, 1024) ||
      !safe_text(result.account.display_name, 1024))
    return fail(oauth_error::invalid_response);
  if (previous && (previous->subject_id != result.account.subject_id ||
                    previous->upstream_account_id != result.account.upstream_account_id))
    return fail(oauth_error::reauthentication_required);
  // Optional identity claims in an access JWT must agree with the ID token.
  if ((access.contains("sub") && string_field(access, "sub") != result.account.subject_id) ||
      (access.contains("iss") && string_field(access, "iss") != issuer))
    return fail(previous ? oauth_error::reauthentication_required : oauth_error::invalid_response);
  const auto access_auth = access.find(auth_claim);
  if (access_auth != access.end() && access_auth->is_object() &&
      access_auth->contains("chatgpt_account_id") &&
      string_field(*access_auth, "chatgpt_account_id") != result.account.upstream_account_id)
    return fail(oauth_error::invalid_response);
  result.account.key.provider = provider;
  return result;
}
} // namespace codex_detail

namespace {
class codex_refresher final : public oauth_token_refresher {
  codex_oauth_options options_;
  std::shared_ptr<http_client> http_;

public:
  codex_refresher(codex_oauth_options options, std::shared_ptr<http_client> http)
      : options_(std::move(options)), http_(codex_detail::transport(std::move(http))) {
    codex_detail::validate(options_);
  }
  std::string provider_id() const override {
    return codex_detail::provider;
  }
  oauth_refresh_result refresh(const oauth_account_info& account, const oauth_tokens& previous,
    std::stop_token stop) override {
    using namespace codex_detail;
    if (account.key.provider != provider || !safe_text(previous.refresh_token, 256 * 1024) ||
        !safe_text(account.subject_id, 1024) || !safe_text(account.upstream_account_id, 1024))
      return { .error = oauth_error::invalid_argument };
    const auto started = std::chrono::system_clock::now();
    const auto response = request(*http_,
      options_,
      token_post(
        options_, "grant_type=refresh_token&refresh_token=" + encode(previous.refresh_token)),
      stop);
    if (response.error)
      return { .error = response.error };
    if (!response.success()) {
      const auto name = error_name(response.body);
      if ((response.status == 400 || response.status == 401) &&
          (name == "invalid_grant" || name == "refresh_token_expired" ||
            name == "refresh_token_reused" || name == "refresh_token_invalidated"))
        return { .error = oauth_error::reauthentication_required };
      return { .error =
                 stop.stop_requested() ? oauth_error::cancelled : oauth_error::refresh_failed };
    }
    auto parsed = decode_tokens(response.body, options_, started, &account);
    return { std::move(parsed.tokens), parsed.error };
  }
};
} // namespace
std::shared_ptr<oauth_token_refresher> make_codex_oauth_refresher(
  codex_oauth_options options, std::shared_ptr<http_client> http) {
  return std::make_shared<codex_refresher>(std::move(options), std::move(http));
}
WUWE_NAMESPACE_END
