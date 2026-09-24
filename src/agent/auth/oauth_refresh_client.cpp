#include <wuwe/agent/auth/oauth_refresh_client.h>

#include <algorithm>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <utility>
#include <wuwe/net/default_http_client.h>

WUWE_NAMESPACE_BEGIN
namespace {
bool control(std::string_view text) {
  return std::any_of(text.begin(), text.end(), [](unsigned char c) { return c < 32 || c == 127; });
}
std::string form_encode(std::string_view text) {
  constexpr char hex[] = "0123456789ABCDEF";
  std::string result;
  for (unsigned char c : text) {
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

std::string ascii_lower(std::string text) {
  for (auto& c : text) {
    if (c >= 'A' && c <= 'Z')
      c = static_cast<char>(c - 'A' + 'a');
  }
  return text;
}
bool terminal_error(const nlohmann::json& data) {
  if (!data.is_object())
    return false;
  auto found = data.find("error");
  if (found == data.end())
    found = data.find("code");
  if (found == data.end())
    return false;
  const auto* value = &*found;
  if (value->is_object()) {
    const auto code = value->find("code");
    if (code == value->end())
      return false;
    value = &*code;
  }
  if (!value->is_string())
    return false;
  const auto code = ascii_lower(value->get<std::string>());
  return code == "invalid_grant" || code == "refresh_token_expired" ||
         code == "refresh_token_reused" || code == "refresh_token_invalidated";
}
} // namespace

oauth_refresh_client::oauth_refresh_client(
  oauth_refresh_endpoint endpoint, std::shared_ptr<http_client> http)
    : endpoint_(std::move(endpoint)), http_(std::move(http)) {
  const auto& url = endpoint_.token_url;
  const auto slash = url.find('/', 8);
  const auto host =
    url.size() >= 8 ? url.substr(8, slash == std::string::npos ? slash : slash - 8) : "";
  if (endpoint_.provider_id.empty() || control(endpoint_.provider_id) ||
      endpoint_.client_id.empty() || control(endpoint_.client_id) || !url.starts_with("https://") ||
      host.empty() || host.find('@') != std::string::npos || control(url) ||
      url.find_first_of(" ?#\\") != std::string::npos || endpoint_.timeout_ms <= 0 ||
      endpoint_.max_response_bytes == 0)
    throw std::invalid_argument("Invalid OAuth refresh endpoint");
  if (!http_)
    http_ = std::make_shared<default_http_client>();
}
std::string oauth_refresh_client::provider_id() const {
  return endpoint_.provider_id;
}

oauth_refresh_result oauth_refresh_client::refresh(
  const oauth_account_info& account, const oauth_tokens& previous, std::stop_token stop) {
  const auto fail = [](oauth_error error) { return oauth_refresh_result { .error = error }; };
  if (stop.stop_requested())
    return fail(oauth_error::cancelled);
  if (account.key.provider != endpoint_.provider_id || previous.refresh_token.empty() ||
      previous.refresh_token.size() > 256 * 1024 || control(previous.refresh_token))
    return fail(oauth_error::invalid_argument);
  const auto started = std::chrono::system_clock::now();
  const auto deadline =
    std::chrono::steady_clock::now() + std::chrono::milliseconds(endpoint_.timeout_ms);
  const http_request request {
    .method = "POST",
    .url = endpoint_.token_url,
    .headers = { { "Content-Type", "application/x-www-form-urlencoded" },
      { "Accept", "application/json" } },
    .body = "grant_type=refresh_token&client_id=" + form_encode(endpoint_.client_id) +
            "&refresh_token=" + form_encode(previous.refresh_token),
    .timeout = endpoint_.timeout_ms,
    .follow_redirects = false,
  };
  std::string body;
  bool too_large = false;
  http_response response;
  try {
    response = http_->send_stream(
      request,
      [&](std::string_view chunk) {
        if (std::chrono::steady_clock::now() >= deadline)
          return false;
        if (chunk.size() > endpoint_.max_response_bytes - body.size()) {
          too_large = true;
          return false;
        }
        body.append(chunk);
        return true;
      },
      stop);
  }
  catch (...) {
    return fail(oauth_error::refresh_failed);
  }
  // A complete successful rotation must be returned to the manager even when
  // stop arrives at completion, so it can durably save the new refresh token.
  if (too_large)
    return fail(oauth_error::invalid_response);
  const bool transport_failed =
    response.transport_error ||
    (response.error_code && response.error_code.category() != http_status_category());
  if (transport_failed || response.error_code || response.status_code < 200 ||
      response.status_code >= 300) {
    if (stop.stop_requested())
      return fail(oauth_error::cancelled);
    const auto error_body = nlohmann::json::parse(body, nullptr, false);
    if (!transport_failed && (response.status_code == 400 || response.status_code == 401) &&
        terminal_error(error_body))
      return fail(oauth_error::reauthentication_required);
    return fail(oauth_error::refresh_failed);
  }
  const auto data = nlohmann::json::parse(body, nullptr, false);
  if (!data.is_object())
    return fail(oauth_error::invalid_response);
  if (data.contains("error"))
    return fail(oauth_error::invalid_response);
  const auto access = data.find("access_token"), expires = data.find("expires_in");
  if (access == data.end() || !access->is_string() ||
      access->get_ref<const std::string&>().empty() || expires == data.end() ||
      !expires->is_number_integer() || *expires <= 0 || *expires > 31 * 24 * 60 * 60)
    return fail(oauth_error::invalid_response);
  const auto type = data.find("token_type");
  if (type != data.end() &&
      (!type->is_string() || ascii_lower(type->get<std::string>()) != "bearer"))
    return fail(oauth_error::invalid_response);
  oauth_refresh_result result;
  result.tokens.access_token = access->get<std::string>();
  result.tokens.expires_at = started + std::chrono::seconds(expires->get<int>());
  for (auto pair : { std::pair { "refresh_token", &result.tokens.refresh_token },
         std::pair { "id_token", &result.tokens.id_token } }) {
    const auto field = data.find(pair.first);
    if (field != data.end()) {
      if (!field->is_string() || field->get_ref<const std::string&>().empty())
        return fail(oauth_error::invalid_response);
      *pair.second = field->get<std::string>();
    }
  }
  if (control(result.tokens.access_token) || control(result.tokens.refresh_token) ||
      control(result.tokens.id_token))
    return fail(oauth_error::invalid_response);
  return result;
}
WUWE_NAMESPACE_END
