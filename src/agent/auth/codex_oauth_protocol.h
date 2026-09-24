#pragma once

#include <nlohmann/json.hpp>
#include <wuwe/agent/auth/codex_oauth.h>
#include <wuwe/net/http_client.h>

WUWE_NAMESPACE_BEGIN
namespace codex_detail {
using json = nlohmann::json;
inline constexpr const char* provider = "codex_oauth";
inline constexpr const char* issuer = "https://auth.openai.com";
inline constexpr const char* token_url = "https://auth.openai.com/oauth/token";
inline constexpr const char* callback_url = "https://auth.openai.com/deviceauth/callback";
inline constexpr const char* auth_claim = "https://api.openai.com/auth";

bool safe_text(std::string_view text, std::size_t limit, bool required = true);
std::string string_field(const json& object, const char* key);
std::string encode(std::string_view value);
std::string random_id();
void validate(const codex_oauth_options& options);
std::shared_ptr<http_client> transport(std::shared_ptr<http_client> http);
json parse(std::string_view body);
std::string error_name(const json& body);
std::optional<std::chrono::seconds> seconds_field(const json& value, std::int64_t maximum);

struct reply {
  json body;
  int status { 0 };
  std::chrono::seconds retry_after { 0 };
  std::error_code error; // Transport/parsing only; caller interprets HTTP status.
  bool success() const {
    return !error && status >= 200 && status < 300;
  }
};
reply request(http_client& http, const codex_oauth_options& options, http_request request,
  std::stop_token stop);
http_request json_post(std::string url, json body);
http_request token_post(const codex_oauth_options& options, std::string form);
std::vector<std::pair<std::string, std::string>> account_headers(const oauth_access_result& access,
  const std::string& originator, const std::string& version, std::string accept);
struct token_result {
  oauth_tokens tokens;
  oauth_account_info account;
  std::error_code error;
};
// Only decode claims received directly from the trusted TLS token endpoint.
// This is not a JWT signature verifier for arbitrary externally supplied tokens.
token_result decode_tokens(const json& body, const codex_oauth_options& options,
  std::chrono::system_clock::time_point started, const oauth_account_info* previous = nullptr);
} // namespace codex_detail
WUWE_NAMESPACE_END
