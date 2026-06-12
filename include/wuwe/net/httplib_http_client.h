#ifndef WUWE_NET_HTTPLIB_HTTP_CLIENT_H
#define WUWE_NET_HTTPLIB_HTTP_CLIENT_H

#include <wuwe/common/wuwe_fwd.h>
#include <wuwe/net/http_client.h>

WUWE_NAMESPACE_BEGIN

class httplib_http_client final : public http_client {
public:
  http_response send(const http_request& request) override;
  http_response send_stream(
    const http_request& request,
    const http_stream_chunk_callback& on_chunk,
    std::stop_token stop_token = {}) override;
};

WUWE_NAMESPACE_END

#endif // WUWE_NET_HTTPLIB_HTTP_CLIENT_H
