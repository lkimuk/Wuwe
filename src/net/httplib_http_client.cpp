#include <wuwe/net/httplib_http_client.h>

#include <wuwe/net/transport_error.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include <httplib/httplib.h>

WUWE_NAMESPACE_BEGIN

namespace {

struct parsed_url {
  std::string scheme_host_port;
  std::string path;
  bool uses_https = false;
};

struct parsed_proxy_url {
  std::string host;
  int port = 0;
};

std::string normalize_http_method(std::string method) {
  std::transform(method.begin(), method.end(), method.begin(), [](unsigned char ch) {
    return static_cast<char>(std::toupper(ch));
  });
  return method;
}

bool is_supported_method(const std::string& method) {
  return method == "GET" || method == "POST" || method == "PUT" ||
         method == "PATCH" || method == "DELETE" || method == "HEAD" ||
         method == "OPTIONS";
}

std::optional<parsed_url> parse_url(const std::string& url) {
  const auto scheme_end = url.find("://");
  if (scheme_end == std::string::npos || scheme_end == 0) {
    return std::nullopt;
  }

  const auto authority_begin = scheme_end + 3;
  auto scheme = url.substr(0, scheme_end);
  std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  const auto path_begin = url.find_first_of("/?", authority_begin);
  const auto fragment_begin = url.find('#', authority_begin);
  const auto end = fragment_begin == std::string::npos ? url.size() : fragment_begin;
  parsed_url parsed;
  parsed.scheme_host_port = path_begin == std::string::npos || path_begin > end
                              ? url.substr(0, end)
                              : url.substr(0, path_begin);
  parsed.uses_https = scheme == "https";
  if (path_begin == std::string::npos) {
    parsed.path = "/";
  }
  else if (url[path_begin] == '/') {
    parsed.path = url.substr(path_begin, end - path_begin);
  }
  else {
    parsed.path = "/" + url.substr(path_begin, end - path_begin);
  }
  return parsed;
}

std::optional<parsed_proxy_url> parse_proxy_url(const std::string& url) {
  auto scheme_end = url.find("://");
  const bool has_scheme = scheme_end != std::string::npos;
  const auto authority_begin = has_scheme ? scheme_end + 3 : 0;
  const auto scheme = has_scheme ? url.substr(0, scheme_end) : std::string {};
  auto authority_end = url.find_first_of("/?#", authority_begin);
  if (authority_end == std::string::npos) {
    authority_end = url.size();
  }
  if (authority_end <= authority_begin) {
    return std::nullopt;
  }

  auto authority = url.substr(authority_begin, authority_end - authority_begin);
  const auto userinfo_end = authority.rfind('@');
  if (userinfo_end != std::string::npos) {
    authority = authority.substr(userinfo_end + 1);
  }

  const auto port_separator = authority.rfind(':');
  parsed_proxy_url parsed;
  if (port_separator == std::string::npos) {
    parsed.host = authority;
    parsed.port = scheme == "https" ? 443 : 80;
  }
  else {
    parsed.host = authority.substr(0, port_separator);
    try {
      parsed.port = std::stoi(authority.substr(port_separator + 1));
    }
    catch (...) {
      return std::nullopt;
    }
  }

  if (parsed.host.empty() || parsed.port <= 0 || parsed.port > 65535) {
    return std::nullopt;
  }
  return parsed;
}

httplib::Headers make_headers(const http_request& request) {
  httplib::Headers headers;
  for (const auto& [key, value] : request.headers) {
    headers.emplace(key, value);
  }
  if (!request.trace_id.empty() && headers.find("X-Request-Id") == headers.end()) {
    headers.emplace("X-Request-Id", request.trace_id);
  }
  return headers;
}

std::vector<http_header> make_response_headers(const httplib::Headers& headers) {
  std::vector<http_header> result;
  result.reserve(headers.size());
  for (const auto& [name, value] : headers) {
    result.push_back({ name, value });
  }
  return result;
}

int effective_total_timeout_ms(const http_request& request) {
  return request.timeouts.total_ms > 0 ? request.timeouts.total_ms : request.timeout;
}

void configure_client(httplib::Client& client, const http_request& request) {
  client.set_follow_location(request.follow_redirects);
  client.set_default_headers({ { "User-Agent", "Wuwe/1.0" } });
  const int total_timeout = effective_total_timeout_ms(request);
  if (total_timeout > 0) {
    client.set_max_timeout(std::chrono::milliseconds(total_timeout));
  }
  if (request.timeouts.connect_ms > 0) {
    client.set_connection_timeout(std::chrono::milliseconds(request.timeouts.connect_ms));
  }
  else if (request.timeout > 0) {
    client.set_connection_timeout(std::chrono::milliseconds(request.timeout));
  }
  if (request.timeouts.read_ms > 0) {
    client.set_read_timeout(std::chrono::milliseconds(request.timeouts.read_ms));
  }
  else if (request.timeout > 0) {
    client.set_read_timeout(std::chrono::milliseconds(request.timeout));
  }
  if (request.timeouts.write_ms > 0) {
    client.set_write_timeout(std::chrono::milliseconds(request.timeouts.write_ms));
  }
  else if (request.timeout > 0) {
    client.set_write_timeout(std::chrono::milliseconds(request.timeout));
  }
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
  client.enable_server_certificate_verification(
    request.tls.verify_peer && request.tls.verify_host);
  if (!request.tls.ca_file.empty() || !request.tls.ca_directory.empty()) {
    client.set_ca_cert_path(request.tls.ca_file, request.tls.ca_directory);
  }
#endif
  if (request.proxy && !request.proxy->url.empty()) {
    const auto proxy = parse_proxy_url(request.proxy->url);
    if (proxy) {
      client.set_proxy(proxy->host, proxy->port);
      if (!request.proxy->bearer_token.empty()) {
        client.set_proxy_bearer_token_auth(request.proxy->bearer_token);
      }
      else if (!request.proxy->username.empty() || !request.proxy->password.empty()) {
        client.set_proxy_basic_auth(request.proxy->username, request.proxy->password);
      }
    }
  }
}

transport_error map_httplib_error(httplib::Error error) {
  switch (error) {
    case httplib::Error::Success:
      return transport_error::unknown_error;
    case httplib::Error::Connection:
    case httplib::Error::ConnectionClosed:
    case httplib::Error::ProxyConnection:
      return transport_error::couldnt_connect;
    case httplib::Error::Read:
      return transport_error::recv_error;
    case httplib::Error::Write:
      return transport_error::send_error;
    case httplib::Error::ExceedRedirectCount:
      return transport_error::too_many_redirects;
    case httplib::Error::Canceled:
      return transport_error::aborted_by_callback;
    case httplib::Error::SSLConnection:
    case httplib::Error::SSLLoadingCerts:
      return transport_error::ssl_connect_error;
    case httplib::Error::SSLServerVerification:
    case httplib::Error::SSLServerHostnameVerification:
      return transport_error::peer_failed_verification;
    case httplib::Error::ConnectionTimeout:
    case httplib::Error::Timeout:
      return transport_error::operation_timedout;
    case httplib::Error::Compression:
      return transport_error::bad_content_encoding;
    case httplib::Error::ResourceExhaustion:
      return transport_error::out_of_memory;
    case httplib::Error::ExceedMaxPayloadSize:
    case httplib::Error::ExceedUriMaxLength:
      return transport_error::too_large;
    case httplib::Error::InvalidHTTPMethod:
    case httplib::Error::InvalidRequestLine:
    case httplib::Error::InvalidHTTPVersion:
    case httplib::Error::InvalidHeaders:
      return transport_error::bad_function_argument;
    case httplib::Error::HTTPParsing:
      return transport_error::weird_server_reply;
    case httplib::Error::OpenFile:
      return transport_error::file_couldnt_read_file;
    case httplib::Error::UnsupportedAddressFamily:
      return transport_error::unsupported_protocol;
    default:
      return transport_error::unknown_error;
  }
}

http_response make_http_response(const httplib::Result& result, std::string body = {}) {
  if (!result) {
    auto transport_error = make_error_code(map_httplib_error(result.error()));
    return { .error_code = transport_error,
      .transport_error = transport_error,
      .body = std::move(body) };
  }

  http_response response;
  response.body = body.empty() ? result->body : std::move(body);
  response.status_code = result->status;
  response.headers = make_response_headers(result->headers);
  response.error_code = make_error_code(static_cast<http_status_code>(result->status));
  return response;
}

http_response make_transport_failure(transport_error error) {
  const auto code = make_error_code(error);
  return { .error_code = code, .transport_error = code };
}

httplib::Result send_request(httplib::Client& client, const http_request& request) {
  httplib::Request req;
  req.method = normalize_http_method(request.method);
  req.path = parse_url(request.url)->path;
  req.headers = make_headers(request);
  req.body = request.body;
  if (request.max_redirects > 0) {
    req.redirect_count_ = static_cast<std::size_t>(request.max_redirects);
  }
  else if (!request.follow_redirects) {
    req.redirect_count_ = 0;
  }
  return client.send(req);
}

} // namespace

http_response httplib_http_client::send(const http_request& request) {
  const auto parsed = parse_url(request.url);
  if (!parsed) {
    return make_transport_failure(transport_error::url_malformat);
  }
  if (!is_supported_method(normalize_http_method(request.method))) {
    return { .error_code = std::make_error_code(std::errc::invalid_argument) };
  }

#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
  if (parsed->uses_https) {
    return make_transport_failure(transport_error::not_built_in);
  }
#endif

  httplib::Client client(parsed->scheme_host_port);
  configure_client(client, request);
  if (!client.is_valid()) {
    return make_transport_failure(transport_error::unsupported_protocol);
  }

  return make_http_response(send_request(client, request));
}

http_response httplib_http_client::send_stream(
  const http_request& request,
  const http_stream_chunk_callback& on_chunk,
  std::stop_token stop_token) {
  if (stop_token.stop_requested()) {
    return { .error_code = std::make_error_code(std::errc::operation_canceled) };
  }

  const auto parsed = parse_url(request.url);
  if (!parsed) {
    return make_transport_failure(transport_error::url_malformat);
  }

#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
  if (parsed->uses_https) {
    return make_transport_failure(transport_error::not_built_in);
  }
#endif

  httplib::Client client(parsed->scheme_host_port);
  configure_client(client, request);
  if (!client.is_valid()) {
    return make_transport_failure(transport_error::unsupported_protocol);
  }

  std::string body;
  bool aborted = false;

  httplib::Request req;
  req.method = normalize_http_method(request.method);
  if (!is_supported_method(req.method)) {
    return { .error_code = std::make_error_code(std::errc::invalid_argument) };
  }
  req.path = parsed->path;
  req.headers = make_headers(request);
  req.body = request.body;
  if (request.max_redirects > 0) {
    req.redirect_count_ = static_cast<std::size_t>(request.max_redirects);
  }
  else if (!request.follow_redirects) {
    req.redirect_count_ = 0;
  }
  req.content_receiver =
    [&](const char* data, std::size_t data_length, std::uint64_t, std::uint64_t) {
      if (stop_token.stop_requested()) {
        aborted = true;
        return false;
      }

      std::string_view chunk(data, data_length);
      body.append(chunk);
      if (on_chunk && !on_chunk(chunk)) {
        aborted = true;
        return false;
      }
      return true;
    };
  req.download_progress = [&](std::size_t, std::size_t) {
    if (stop_token.stop_requested()) {
      aborted = true;
      return false;
    }
    return true;
  };

  const auto result = client.send(req);
  auto response = make_http_response(result, std::move(body));
  if (aborted && !response.error_code) {
    response.transport_error = make_error_code(transport_error::aborted_by_callback);
    response.error_code = response.transport_error;
  }
  return response;
}

WUWE_NAMESPACE_END
