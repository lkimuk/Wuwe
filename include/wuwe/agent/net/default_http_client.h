#ifndef WUWE_AGENT_NET_DEFAULT_HTTP_CLIENT_H
#define WUWE_AGENT_NET_DEFAULT_HTTP_CLIENT_H

#include <string>

#include <cpr/cpr.h>

#include <wuwe/agent/net/http_client.h>
#include <wuwe/common/wuwe_fwd.h>

WUWE_AGENT_NAMESPACE_BEGIN

class default_http_client final : public http_client {
public:

    http_response send(const http_request& request) override;

private:
    static std::string normalize_http_method(std::string method);
    static cpr::Header make_cpr_headers(const http_request& request);
    static http_response make_http_response(const cpr::Response& response);
};

WUWE_AGENT_NAMESPACE_END

#endif // WUWE_AGENT_NET_DEFAULT_HTTP_CLIENT_H
