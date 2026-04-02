#include <wuwe/agent/net/default_http_client.h>

#include <algorithm>
#include <cctype>
#include <utility>

WUWE_AGENT_NAMESPACE_BEGIN

http_response default_http_client::send(const http_request& request) {
    cpr::Session session;
    session.SetUrl(cpr::Url {request.url});
    session.SetHeader(make_cpr_headers(request));

    if (!request.body.empty()) {
        session.SetBody(cpr::Body {request.body});
    }

    const std::string method = normalize_http_method(request.method);

    if (method == "GET") {
        return make_http_response(session.Get());
    }
    if (method == "POST") {
        return make_http_response(session.Post());
    }
    if (method == "PUT") {
        return make_http_response(session.Put());
    }
    if (method == "PATCH") {
        return make_http_response(session.Patch());
    }
    if (method == "DELETE") {
        return make_http_response(session.Delete());
    }
    if (method == "HEAD") {
        return make_http_response(session.Head());
    }
    if (method == "OPTIONS") {
        return make_http_response(session.Options());
    }

    http_response response;
    response.error_message = "unsupported HTTP method: " + request.method;
    return response;
}

std::string default_http_client::normalize_http_method(std::string method) {
    std::transform(method.begin(), method.end(), method.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return method;
}

cpr::Header default_http_client::make_cpr_headers(const http_request& request) {
    cpr::Header headers;
    for (const auto& [key, value] : request.headers) {
        headers[key] = value;
    }
    return headers;
}

http_response default_http_client::make_http_response(const cpr::Response& response) {
    http_response result;
    result.status_code = static_cast<int>(response.status_code);
    result.body = response.text;
    result.error_message = response.error.message;
    result.ok = response.error.code == cpr::ErrorCode::OK &&
                result.status_code >= 200 &&
                result.status_code < 300;
    return result;
}

WUWE_AGENT_NAMESPACE_END