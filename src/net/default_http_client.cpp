#include <wuwe/net/default_http_client.h>

#include <wuwe/net/transport_error.h>

#include <algorithm>
#include <cctype>
#include <utility>

#include <cpr/cpr.h>

WUWE_NAMESPACE_BEGIN

namespace {

std::string normalize_http_method(std::string method) {
  std::transform(method.begin(), method.end(), method.begin(), [](unsigned char ch) {
    return static_cast<char>(std::toupper(ch));
  });

  return method;
}

cpr::Header make_cpr_headers(const http_request& request) {
  cpr::Header headers;
  for (const auto& [key, value] : request.headers) {
    headers[key] = value;
  }

  return headers;
}

http_response make_http_response(const cpr::Response& response) {
  http_response result;
  result.body = response.text;

  if (response.error.code != cpr::ErrorCode::OK) {
    result.error_code = make_error_code(static_cast<transport_error>(response.error.code));
  }
  else {
    result.error_code = make_error_code(static_cast<http_status_code>(response.status_code));
  }

  return result;
}

} // unnamed namespace

http_response default_http_client::send(const http_request& request) {
  cpr::Session session;
  session.SetUrl(cpr::Url { request.url });
  session.SetHeader(make_cpr_headers(request));
  if (request.timeout > 0) {
    session.SetTimeout(cpr::Timeout { request.timeout });
  }

  if (!request.body.empty()) {
    session.SetBody(cpr::Body { request.body });
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
  response.error_code = std::make_error_code(std::errc::invalid_argument);
  return response;
}

WUWE_NAMESPACE_END
