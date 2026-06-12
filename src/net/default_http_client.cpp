#include <wuwe/net/default_http_client.h>

#include <wuwe/net/cpr_http_client.h>
#include <wuwe/net/httplib_http_client.h>

WUWE_NAMESPACE_BEGIN

namespace {

#if defined(WUWE_DEFAULT_HTTP_BACKEND_HTTPLIB)
using selected_default_http_client = httplib_http_client;
constexpr const char* selected_backend_name = "httplib";
#else
using selected_default_http_client = cpr_http_client;
constexpr const char* selected_backend_name = "cpr";
#endif

} // namespace

const char* default_http_client::backend_name() noexcept {
  return selected_backend_name;
}

http_response default_http_client::send(const http_request& request) {
  selected_default_http_client client;
  return client.send(request);
}

http_response default_http_client::send_stream(
  const http_request& request,
  const http_stream_chunk_callback& on_chunk,
  std::stop_token stop_token) {
  selected_default_http_client client;
  return client.send_stream(request, on_chunk, stop_token);
}

WUWE_NAMESPACE_END
