#ifndef WUWE_AGENT_NET_DEFAULT_HTTP_CLIENT_H
#define WUWE_AGENT_NET_DEFAULT_HTTP_CLIENT_H

#include <string>

#include <wuwe/common/wuwe_fwd.h>
#include <wuwe/net/http_client.h>

WUWE_NAMESPACE_BEGIN

class default_http_client final : public http_client {
public:
  http_response send(const http_request& request) override;
};

WUWE_NAMESPACE_END

#endif // WUWE_AGENT_NET_DEFAULT_HTTP_CLIENT_H
