#ifndef WUWE_AGENT_NET_HTTP_CLIENT_H
#define WUWE_AGENT_NET_HTTP_CLIENT_H

#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <wuwe/common/wuwe_fwd.h>
#include <wuwe/net/http_status_code.h>

WUWE_AGENT_NAMESPACE_BEGIN

struct http_request {
  std::string method;
  std::string url;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  int timeout_ms;
};

struct http_response {
  std::error_code error_code;
  std::string body;
};

class http_client {
public:
  virtual ~http_client() = default;
  virtual http_response send(const http_request &request) = 0;
};

WUWE_AGENT_NAMESPACE_END

#endif // WUWE_AGENT_NET_HTTP_CLIENT_H
