#ifndef WUWE_AGENT_NET_HTTP_CLIENT_H
#define WUWE_AGENT_NET_HTTP_CLIENT_H

#include <functional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <wuwe/common/wuwe_fwd.h>
#include <wuwe/net/http_status_code.h>

WUWE_NAMESPACE_BEGIN

struct http_request {
  std::string method;
  std::string url;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  int timeout;
};

struct http_response {
  std::error_code error_code;
  std::string body;
};

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
