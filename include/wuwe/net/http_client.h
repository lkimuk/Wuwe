#ifndef WUWE_AGENT_NET_HTTP_CLIENT_H
#define WUWE_AGENT_NET_HTTP_CLIENT_H

#include <algorithm>
#include <cctype>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <wuwe/common/wuwe_fwd.h>
#include <wuwe/net/http_status_code.h>

WUWE_NAMESPACE_BEGIN

struct http_header {
  std::string name;
  std::string value;
};

struct http_proxy_options {
  std::string url;
  std::string username;
  std::string password;
  std::string bearer_token;
};

struct http_tls_options {
  bool verify_peer = true;
  bool verify_host = true;
  std::string ca_file;
  std::string ca_directory;
};

struct http_timeout_options {
  int total_ms = 0;
  int connect_ms = 0;
  int read_ms = 0;
  int write_ms = 0;
};

struct http_request {
  std::string method;
  std::string url;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  int timeout = 0;
  http_timeout_options timeouts;
  bool follow_redirects = true;
  int max_redirects = 0;
  std::optional<http_proxy_options> proxy;
  http_tls_options tls;
  std::string trace_id;
};

struct http_response {
  std::error_code error_code;
  std::error_code transport_error;
  int status_code = 0;
  std::vector<http_header> headers;
  std::string body;
};

[[nodiscard]] inline bool http_header_name_equals(
  std::string_view lhs,
  std::string_view rhs) noexcept {
  return lhs.size() == rhs.size() &&
         std::equal(lhs.begin(), lhs.end(), rhs.begin(), [](char a, char b) {
           return std::tolower(static_cast<unsigned char>(a)) ==
                  std::tolower(static_cast<unsigned char>(b));
         });
}

[[nodiscard]] inline std::optional<std::string_view> find_http_header(
  const std::vector<http_header>& headers,
  std::string_view name) noexcept {
  for (const auto& header : headers) {
    if (http_header_name_equals(header.name, name)) {
      return std::string_view { header.value };
    }
  }
  return std::nullopt;
}

[[nodiscard]] inline bool has_http_header(
  const std::vector<http_header>& headers,
  std::string_view name) noexcept {
  return find_http_header(headers, name).has_value();
}

using http_stream_chunk_callback = std::function<bool(std::string_view)>;

class http_client {
public:
  virtual ~http_client() = default;
  virtual http_response send(const http_request& request) = 0;

  virtual http_response send_stream(
    const http_request& request,
    const http_stream_chunk_callback& on_chunk,
    std::stop_token stop_token = {}) {
    if (stop_token.stop_requested()) {
      return { .error_code = std::make_error_code(std::errc::operation_canceled) };
    }

    auto response = send(request);
    if (!response.error_code && on_chunk && !response.body.empty() &&
        !on_chunk(response.body)) {
      response.error_code = std::make_error_code(std::errc::operation_canceled);
    }
    return response;
  }
};

WUWE_NAMESPACE_END

#endif // WUWE_AGENT_NET_HTTP_CLIENT_H
