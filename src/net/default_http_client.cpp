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

void configure_session(cpr::Session& session, const http_request& request) {
  session.SetUrl(cpr::Url { request.url });
  session.SetHeader(make_cpr_headers(request));
  session.SetRedirect(cpr::Redirect { true });
  session.SetUserAgent(cpr::UserAgent { "Wuwe/1.0" });
  session.SetSslOptions(cpr::Ssl(cpr::ssl::NoRevoke { true }));
  if (request.timeout > 0) {
    session.SetTimeout(cpr::Timeout { request.timeout });
  }

  if (!request.body.empty()) {
    session.SetBody(cpr::Body { request.body });
  }
}

http_response make_http_response(const cpr::Response& response, std::string body = {}) {
  http_response result;
  result.body = body.empty() ? response.text : std::move(body);

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
  configure_session(session, request);

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

http_response default_http_client::send_stream(
  const http_request& request,
  const http_stream_chunk_callback& on_chunk,
  std::stop_token stop_token) {
  if (stop_token.stop_requested()) {
    return { .error_code = std::make_error_code(std::errc::operation_canceled) };
  }

  cpr::Session session;
  configure_session(session, request);

  std::string body;
  bool aborted = false;
  session.SetWriteCallback(cpr::WriteCallback(
    [&](std::string_view data, intptr_t) {
      if (stop_token.stop_requested()) {
        aborted = true;
        return false;
      }

      body.append(data);
      if (on_chunk && !on_chunk(data)) {
        aborted = true;
        return false;
      }
      return true;
    }));

  session.SetProgressCallback(cpr::ProgressCallback(
    [&](cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t, intptr_t) {
      if (stop_token.stop_requested()) {
        aborted = true;
        return false;
      }
      return true;
    }));

  const std::string method = normalize_http_method(request.method);
  cpr::Response response;

  if (method == "GET") {
    response = session.Get();
  }
  else if (method == "POST") {
    response = session.Post();
  }
  else if (method == "PUT") {
    response = session.Put();
  }
  else if (method == "PATCH") {
    response = session.Patch();
  }
  else if (method == "DELETE") {
    response = session.Delete();
  }
  else if (method == "HEAD") {
    response = session.Head();
  }
  else if (method == "OPTIONS") {
    response = session.Options();
  }
  else {
    return { .error_code = std::make_error_code(std::errc::invalid_argument) };
  }

  auto result = make_http_response(response, std::move(body));
  if (aborted && !result.error_code) {
    result.error_code = make_error_code(transport_error::aborted_by_callback);
  }
  return result;
}

WUWE_NAMESPACE_END
