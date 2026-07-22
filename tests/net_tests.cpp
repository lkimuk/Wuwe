#include <chrono>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <httplib/httplib.h>

#include <wuwe/net/cpr_http_client.h>
#include <wuwe/net/default_http_client.h>
#include <wuwe/net/httplib_http_client.h>
#include <wuwe/net/transport_error.h>

namespace {

using namespace wuwe;

void require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

class local_http_server {
public:
  local_http_server() {
    server_.Get("/hello", [](const httplib::Request& request, httplib::Response& response) {
      response.set_header("X-Trace-Seen", request.get_header_value("X-Request-Id"));
      response.set_header("X-Reply", "present");
      response.set_content("hello:" + request.get_header_value("X-Test"), "text/plain");
    });
    server_.Post("/echo", [](const httplib::Request& request, httplib::Response& response) {
      response.set_content(request.body, "application/json");
    });
    server_.Get("/stream", [](const httplib::Request&, httplib::Response& response) {
      response.set_content("stream-one\nstream-two\n", "text/plain");
    });
    server_.Get("/slow-stream", [](const httplib::Request&, httplib::Response& response) {
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
      response.set_content("late-body", "text/plain");
    });
    server_.Get("/redirect", [](const httplib::Request&, httplib::Response& response) {
      response.status = 302;
      response.set_header("Location", "/redirect-target");
    });
    server_.Get("/redirect-target", [](const httplib::Request&, httplib::Response& response) {
      response.set_content("redirected", "text/plain");
    });
    server_.Get("/not-found", [](const httplib::Request&, httplib::Response& response) {
      response.status = 404;
      response.set_content("missing", "text/plain");
    });

    port_ = server_.bind_to_any_port("127.0.0.1");
    require(port_ > 0, "local HTTP test server should bind to a port");
    worker_ = std::thread([this] {
      server_.listen_after_bind();
    });
    while (!server_.is_running()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }

  ~local_http_server() {
    server_.stop();
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  std::string url(const std::string& path) const {
    return "http://127.0.0.1:" + std::to_string(port_) + path;
  }

  int port() const {
    return port_;
  }

private:
  httplib::Server server_;
  int port_ {};
  std::thread worker_;
};

class local_proxy_server {
public:
  local_proxy_server() {
    server_.Get(R"(.*)", [](const httplib::Request& request, httplib::Response& response) {
      response.set_header("X-Proxy-Seen", "true");
      response.set_content("proxy:" + request.target, "text/plain");
    });

    port_ = server_.bind_to_any_port("127.0.0.1");
    require(port_ > 0, "local proxy test server should bind to a port");
    worker_ = std::thread([this] {
      server_.listen_after_bind();
    });
    while (!server_.is_running()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }

  ~local_proxy_server() {
    server_.stop();
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  std::string url() const {
    return "http://127.0.0.1:" + std::to_string(port_);
  }

private:
  httplib::Server server_;
  int port_ {};
  std::thread worker_;
};

#ifdef WUWE_NET_TESTS_HAS_OPENSSL
constexpr const char* test_cert_pem = R"(-----BEGIN CERTIFICATE-----
MIIDJTCCAg2gAwIBAgIUDzHLqjyRZD6uvJ4pdSRVqtW3bkEwDQYJKoZIhvcNAQEL
BQAwFDESMBAGA1UEAwwJbG9jYWxob3N0MB4XDTI2MDYxMjAxMjQzOFoXDTM2MDYw
OTAxMjQzOFowFDESMBAGA1UEAwwJbG9jYWxob3N0MIIBIjANBgkqhkiG9w0BAQEF
AAOCAQ8AMIIBCgKCAQEAtke0LFLn7uSLH3x+ET0cmd0KcecFpofZD3fqLRAb/VEp
Ijg3xt2OBzl+P/tK0bKS4vEmx30oQPS9S+PdCOEu4hzn5FCHLFHwsMdIBDG+V3oQ
nrEj0wMp/9RtcIxB3D1W8H98DjO1FUF4vtPlbQBF7TUF6miEGx3VCtj8+jDMrbyK
5VCVxrsYYkyBcH82ehU4W32ZkU66U/dB+GStJSVxYEtJkWpeCBamKLsdYJXMwN8v
aeR/YxqJq7pfaqDDZFkjrfpBAOj/AP9w3zxlfGOSwgrm6lXqaKsFHUY9vdcBhBQs
UWwT1VTfKoz9Qe8ilgiDhJWSZrPJVdSRtlIDUFWMyQIDAQABo28wbTAdBgNVHQ4E
FgQU2QdLm2bPsa4R4uBXKC0lxdQsvoQwHwYDVR0jBBgwFoAU2QdLm2bPsa4R4uBX
KC0lxdQsvoQwDwYDVR0TAQH/BAUwAwEB/zAaBgNVHREEEzARgglsb2NhbGhvc3SH
BH8AAAEwDQYJKoZIhvcNAQELBQADggEBADgXzhLIB7qqFOnFjOcNIzEJCNm4onh5
8wvdt6+hj5U2MVh2egsZHo8ChNW57hU+Y8wYS77w7XBZedZiUew8XuenSSxEdyhj
XeisIPvFpUK3jZrVpoVMLoTsLGv/lSNt0FZIcTU7HKYFGdeb8/nuII5WHRd1R95x
5S76jxYCdseaz2HYO4yIN38U0cDuIey0FUyRFhmD4BPfajzik1Y1fa9hofKzAu/Y
JBLh0x2AihT3WtTS3qu7kvWFizsF1UyD6yiqzO5yvO0gcPyd8rnYd0YZFKdJ3cwp
d9jocPGFEWgkaN3ij5+rZ+h+3GKTlnnP+62jzOWfnKvUXeUPqEP0KIw=
-----END CERTIFICATE-----)";

constexpr const char* test_key_pem = R"(-----BEGIN PRIVATE KEY-----
MIIEvgIBADANBgkqhkiG9w0BAQEFAASCBKgwggSkAgEAAoIBAQC2R7QsUufu5Isf
fH4RPRyZ3Qpx5wWmh9kPd+otEBv9USkiODfG3Y4HOX4/+0rRspLi8SbHfShA9L1L
490I4S7iHOfkUIcsUfCwx0gEMb5XehCesSPTAyn/1G1wjEHcPVbwf3wOM7UVQXi+
0+VtAEXtNQXqaIQbHdUK2Pz6MMytvIrlUJXGuxhiTIFwfzZ6FThbfZmRTrpT90H4
ZK0lJXFgS0mRal4IFqYoux1glczA3y9p5H9jGomrul9qoMNkWSOt+kEA6P8A/3Df
PGV8Y5LCCubqVepoqwUdRj291wGEFCxRbBPVVN8qjP1B7yKWCIOElZJms8lV1JG2
UgNQVYzJAgMBAAECggEAC7JnO+7oBjrxOIiHGHkY9ECscmm2QZz3D1iNFO4zNZq3
LoR4A9Fk007brKRmBTXV2i1KZUPBSQXLfeq13OOCXC0prkuCmRP7A4UOOIKuqbNi
DKTmXRyXXdzWVwIRGd5vzUgJAGpOBPqqEmjLkKaTVijwY73oOEs+SA9rkGyAPee/
LIB3lUiwuIz5T7F8Z0DUGEriFuODZIplP3swNqLJo/It+39F0LE/uygBSSoHSaa0
ClBLBR4oj+tGWn2KlbWbo+t2s9McrUtF5mWBUfbb2w1/w0FZRXIgbweXqce+UHEc
yWeSPvaFo2+oETklWLeLNFPVo/MqAa9zzX6YR+zLVwKBgQDhLqDcokPyEg9Pmbe1
3rXB4illNx1QPBXZZTQqOfkOze1bTX6dX6io13utpbzPkNHnEkUqnkFfOTwLucsI
KZtR9pfiyVKkoQHh14GfYNMZ9BDJCaVw51hXH7kslwt+Vuj06JmzLTFgP5yaMU29
m7+iAJ7r+R/DDsAgtUsZmP3hewKBgQDPOfrtHMdhKaJIOt3GoFmwateyV5mFHhJs
gj2Gu+AQeIHKAT/zbYgMNN3kOtf8z8B65V/hqgXlcX+9p2fxZA/yEYfEUECFwKD9
cWE/VmMKmZrcunwnKC5gXkavu4sDFs11aECMRRzqVIEz7Si9lhCEgofVV/Mr0Jf8
7E5cVpStiwKBgQCzlIUTD5ESNxbgy103/GGFOsD6iCanexONqOkeF9eo189H9hhY
lxYheJ+Yj0lxWzQajHZ+k3Dc6P8a9tOVMeE9T2Q3p4hx5DllC4HDQet4kizktv2q
ecT4zkLV7atr2RG1Zt5Uh2EOOgzA5zrxUIlWQBp9Y9LRsyzDqPE4e8tUiwKBgGp2
uUn0jSKIB03goGwZmbqfSa3gf4j6iDCjQQTlpRoRL20e80IXNdw/lPhamvjRq2v4
SChh96GHjD9dsHM+G0scYoojSOLuskdDZtjpgvzBKeTZEkvzws/T37ENQ0AVCP2W
0ALAxzhErhSFdXbhkB7kCPE8vDv4cP2KUj/yY3Q9AoGBAMEOyYxwhACpGFvYPImq
TAMHNAg5i5/qnEInQCFsVZuv8ICCt6avTgEhDlUi2A589XKV5Ci07/D3L7+jEcp+
iMvfkL1gEejFF+r7OP7O3iNKT0KOyBilaDNhRZHj6U+GFYQJGQV25gkJR441oODa
9VEoJLbJMbZCSNEuroJ4GvVd
-----END PRIVATE KEY-----)";

class local_https_server {
public:
  local_https_server()
      : server_(httplib::SSLServer::PemMemory {
          .cert_pem = test_cert_pem,
          .key_pem = test_key_pem,
        }) {
    require(server_.is_valid(), "local HTTPS test server should initialize");
    server_.Get("/secure", [](const httplib::Request&, httplib::Response& response) {
      response.set_content("secure", "text/plain");
    });

    port_ = server_.bind_to_any_port("127.0.0.1");
    require(port_ > 0, "local HTTPS test server should bind to a port");
    worker_ = std::thread([this] {
      server_.listen_after_bind();
    });
    while (!server_.is_running()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }

  ~local_https_server() {
    server_.stop();
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  std::string url(const std::string& path) const {
    return "https://127.0.0.1:" + std::to_string(port_) + path;
  }

private:
  httplib::SSLServer server_;
  int port_ {};
  std::thread worker_;
};
#endif

std::optional<transport_error> as_transport_error(std::error_code code) {
  if (code.category() != transport_error_category()) {
    return std::nullopt;
  }
  return static_cast<transport_error>(code.value());
}

bool is_transport_connection_error(std::error_code code) {
  const auto transport = as_transport_error(code);
  return transport && is_connection_error(*transport);
}

bool is_transport_availability_error(std::error_code code) {
  const auto transport = as_transport_error(code);
  return transport && (is_connection_error(*transport) || is_timeout(*transport));
}

bool is_transport_tls_error(std::error_code code) {
  const auto transport = as_transport_error(code);
  return transport && is_tls_error(*transport);
}

std::string error_debug_string(std::error_code code) {
  return code.category().name() + (":" + std::to_string(code.value()) + ":" + code.message());
}

void exercise_client(http_client& client, const local_http_server& server) {
  const auto get = client.send({
    .method = "GET",
    .url = server.url("/hello"),
    .headers = { { "X-Test", "backend" } },
    .timeout = 5000,
  });
  require(!get.error_code, "GET should succeed: " + get.error_code.message());
  require(!get.transport_error, "GET should not carry a transport error");
  require(get.status_code == 200, "GET should expose HTTP status code 200");
  require(find_http_header(get.headers, "x-reply") == "present",
    "GET should expose response headers with case-insensitive lookup");
  require(get.body == "hello:backend", "GET should return response body");

  const auto traced = client.send({
    .method = "GET",
    .url = server.url("/hello"),
    .headers = { { "X-Test", "trace" } },
    .timeout = 5000,
    .trace_id = "trace-123",
  });
  require(!traced.error_code, "traced GET should succeed: " + traced.error_code.message());
  require(find_http_header(traced.headers, "x-trace-seen") == "trace-123",
    "trace_id should be sent as X-Request-Id when caller did not provide it");

  const auto caller_trace = client.send({
    .method = "GET",
    .url = server.url("/hello"),
    .headers = {
      { "X-Test", "trace" },
      { "x-request-id", "caller-trace" },
    },
    .timeout = 5000,
    .trace_id = "generated-trace",
  });
  require(!caller_trace.error_code,
    "caller trace GET should succeed: " + caller_trace.error_code.message());
  require(find_http_header(caller_trace.headers, "X-Trace-Seen") == "caller-trace",
    "trace_id should not override an explicit request id header");

  const auto post = client.send({
    .method = "POST",
    .url = server.url("/echo"),
    .headers = { { "Content-Type", "application/json" } },
    .body = R"({"ok":true})",
    .timeout = 5000,
  });
  require(!post.error_code, "POST should succeed: " + post.error_code.message());
  require(post.body == R"({"ok":true})", "POST should send and receive body");

  const auto missing = client.send({
    .method = "GET",
    .url = server.url("/not-found"),
    .timeout = 5000,
  });
  require(missing.error_code == http_status_code::not_found,
    "HTTP 404 should map to http_status_code::not_found");
  require(!missing.transport_error, "HTTP 404 should not be a transport error");
  require(missing.status_code == 404, "HTTP 404 should expose status_code");

  const auto redirected = client.send({
    .method = "GET",
    .url = server.url("/redirect"),
    .timeout = 5000,
  });
  require(!redirected.error_code, "redirect should succeed: " + redirected.error_code.message());
  require(redirected.body == "redirected", "redirect should return target body");

  const auto not_redirected = client.send({
    .method = "GET",
    .url = server.url("/redirect"),
    .timeout = 5000,
    .follow_redirects = false,
  });
  require(not_redirected.error_code == http_status_code::found,
    "disabled redirects should expose the 302 status as the compatibility error_code");
  require(not_redirected.status_code == 302, "disabled redirects should expose status_code 302");
  require(find_http_header(not_redirected.headers, "location") == "/redirect-target",
    "disabled redirects should expose Location header");

  std::string streamed;
  const auto stream = client.send_stream(
    {
      .method = "GET",
      .url = server.url("/stream"),
      .timeout = 5000,
    },
    [&](std::string_view chunk) {
      streamed.append(chunk);
      return true;
    });
  require(!stream.error_code, "streaming GET should succeed: " + stream.error_code.message());
  require(stream.body == "stream-one\nstream-two\n",
    "streaming GET should preserve full body");
  require(streamed == stream.body, "streaming callback should receive the response body");

  const auto aborted = client.send_stream(
    {
      .method = "GET",
      .url = server.url("/stream"),
      .timeout = 5000,
    },
    [](std::string_view) {
      return false;
    });
  require(!!aborted.error_code, "callback abort should return an error");

  std::stop_source stop_source;
  std::thread stopper([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    stop_source.request_stop();
  });
  const auto slow = client.send_stream(
    {
      .method = "GET",
      .url = server.url("/slow-stream"),
      .timeout = 5000,
    },
    [](std::string_view) {
      return true;
    },
    stop_source.get_token());
  if (stopper.joinable()) {
    stopper.join();
  }
  require(!!slow.error_code, "stop token should abort a slow stream");

  const auto malformed = client.send({
    .method = "GET",
    .url = "not-a-url",
    .timeout = 100,
  });
  require(!!malformed.error_code,
    "malformed URL should return an error");

  const auto unsupported_method = client.send({
    .method = "TRACE",
    .url = server.url("/hello"),
    .timeout = 5000,
  });
  require(unsupported_method.error_code == std::errc::invalid_argument,
    "unsupported HTTP method should map to std::errc::invalid_argument: " +
      error_debug_string(unsupported_method.error_code));
}

void exercise_connection_failure(http_client& client) {
  int port = 0;
  {
    local_http_server server;
    port = server.port();
  }

  const auto response = client.send({
    .method = "GET",
    .url = "http://127.0.0.1:" + std::to_string(port) + "/closed",
    .timeout = 100,
  });
  require(is_transport_availability_error(response.error_code),
    "closed local port should map to a connection or timeout transport error: " +
      response.error_code.message());
  require(response.transport_error == response.error_code,
    "connection failure should populate transport_error separately");
}

void exercise_http_proxy(http_client& client) {
  local_proxy_server proxy;
  const auto response = client.send({
    .method = "GET",
    .url = "http://wuwe-proxy-test.invalid/proxy-target",
    .timeout = 5000,
    .proxy = http_proxy_options {
      .url = proxy.url(),
    },
  });
  require(!response.error_code,
    "HTTP proxy request should be served by the configured proxy: " +
      response.error_code.message());
  require(response.body.find("proxy:") == 0,
    "HTTP proxy response should come from the local proxy server");
  require(find_http_header(response.headers, "x-proxy-seen") == "true",
    "HTTP proxy response should expose proxy response headers");
}

void test_cpr_http_client() {
  local_http_server server;
  cpr_http_client client;
  exercise_client(client, server);
  exercise_connection_failure(client);
  exercise_http_proxy(client);
}

void test_httplib_http_client() {
  local_http_server server;
  httplib_http_client client;
  exercise_client(client, server);
  exercise_connection_failure(client);
  exercise_http_proxy(client);
}

void test_default_http_client_reports_backend() {
  const std::string backend = default_http_client::backend_name();
  require(backend == "cpr" || backend == "httplib",
    "default HTTP backend should be cpr or httplib");
}

void test_default_http_client_uses_selected_backend() {
  local_http_server server;
  default_http_client client;
  exercise_client(client, server);
}

void test_httplib_https_capability_is_explicit() {
#ifndef WUWE_NET_TESTS_HAS_HTTPLIB_HTTPS
  httplib_http_client client;
  const auto response = client.send({
    .method = "GET",
    .url = "https://127.0.0.1:1/",
    .timeout = 100,
  });
  require(response.error_code == make_error_code(transport_error::not_built_in),
    "httplib HTTPS should report not_built_in when OpenSSL support is disabled: " +
      error_debug_string(response.error_code));
  require(response.transport_error == response.error_code,
    "httplib HTTPS capability failure should be reported as a transport error");
#endif
}

#ifdef WUWE_NET_TESTS_HAS_OPENSSL
void test_tls_verification_errors_are_classified() {
  local_https_server server;

  cpr_http_client cpr;
  const auto cpr_response = cpr.send({
    .method = "GET",
    .url = server.url("/secure"),
    .timeout = 5000,
  });
  require(is_transport_tls_error(cpr_response.error_code),
    "cpr HTTPS self-signed certificate failure should map to TLS error: " +
      cpr_response.error_code.message());

#ifdef WUWE_NET_TESTS_HAS_HTTPLIB_HTTPS
  httplib_http_client httplib;
  const auto httplib_response = httplib.send({
    .method = "GET",
    .url = server.url("/secure"),
    .timeout = 5000,
  });
  require(is_transport_tls_error(httplib_response.error_code),
    "httplib HTTPS self-signed certificate failure should map to TLS error: " +
      httplib_response.error_code.message());
#endif
}

void test_tls_verification_can_be_disabled() {
  local_https_server server;

  cpr_http_client cpr;
  const auto cpr_response = cpr.send({
    .method = "GET",
    .url = server.url("/secure"),
    .timeout = 5000,
    .tls = {
      .verify_peer = false,
      .verify_host = false,
    },
  });
  require(!cpr_response.error_code,
    "cpr HTTPS self-signed certificate should succeed when verification is disabled: " +
      cpr_response.error_code.message());
  require(cpr_response.body == "secure", "cpr HTTPS should return secure body");

#ifdef WUWE_NET_TESTS_HAS_HTTPLIB_HTTPS
  httplib_http_client httplib;
  const auto httplib_response = httplib.send({
    .method = "GET",
    .url = server.url("/secure"),
    .timeout = 5000,
    .tls = {
      .verify_peer = false,
      .verify_host = false,
    },
  });
  require(!httplib_response.error_code,
    "httplib HTTPS self-signed certificate should succeed when verification is disabled: " +
      httplib_response.error_code.message());
  require(httplib_response.body == "secure", "httplib HTTPS should return secure body");
#endif
}
#endif

std::string env_value(const char* name) {
#ifdef _WIN32
  char* value {};
  std::size_t size {};
  if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
    return {};
  }
  std::string result(value);
  std::free(value);
  return result;
#else
  const auto* value = std::getenv(name);
  return value == nullptr ? std::string {} : std::string(value);
#endif
}

void test_live_https_when_configured() {
  const auto url = env_value("WUWE_TEST_HTTPS_URL");
  if (url.empty()) {
    return;
  }

  cpr_http_client cpr;
  const auto cpr_response = cpr.send({
    .method = "GET",
    .url = url,
    .timeout = 10000,
  });
  require(!cpr_response.error_code,
    "cpr live HTTPS request should succeed: " + cpr_response.error_code.message());

#ifdef WUWE_NET_TESTS_HAS_HTTPLIB_HTTPS
  httplib_http_client httplib;
  const auto httplib_response = httplib.send({
    .method = "GET",
    .url = url,
    .timeout = 10000,
  });
  require(!httplib_response.error_code,
    "httplib live HTTPS request should succeed: " + httplib_response.error_code.message());
#endif
}

void run(const char* name, void (*test)()) {
  test();
  std::cout << "[PASS] " << name << '\n';
}

} // namespace

int main() {
  try {
    run("cpr HTTP client", test_cpr_http_client);
    run("httplib HTTP client", test_httplib_http_client);
    run("default HTTP client reports backend", test_default_http_client_reports_backend);
    run("default HTTP client uses selected backend", test_default_http_client_uses_selected_backend);
    run("httplib HTTPS capability is explicit", test_httplib_https_capability_is_explicit);
#ifdef WUWE_NET_TESTS_HAS_OPENSSL
    run("TLS verification errors are classified", test_tls_verification_errors_are_classified);
    run("TLS verification can be disabled", test_tls_verification_can_be_disabled);
#endif
    run("live HTTPS when configured", test_live_https_when_configured);
  }
  catch (const std::exception& ex) {
    std::cerr << "[FAIL] " << ex.what() << '\n';
    return 1;
  }

  return 0;
}
