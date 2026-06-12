#include <wuwe/net/transport_error.h>

#include <system_error>

WUWE_NAMESPACE_BEGIN

namespace {

template <typename T> struct constant_init {
  union {
    T obj;
  };
  constexpr constant_init() noexcept : obj() {}

  ~constant_init() { /* do nothing, union object is not destroyed */ }
};

struct transport_error_category_impl final : public std::error_category {
public:
  const char *name() const noexcept final {
    return "transport";
  }

  std::string message(int code) const final {
    switch (static_cast<transport_error>(code)) {
      case transport_error::unsupported_protocol:
        return "unsupported protocol";
      case transport_error::failed_init:
        return "failed to initialize transport";
      case transport_error::url_malformat:
        return "malformed URL";
      case transport_error::couldnt_resolve_proxy:
        return "could not resolve proxy";
      case transport_error::couldnt_resolve_host:
        return "could not resolve host";
      case transport_error::couldnt_connect:
        return "could not connect";
      case transport_error::weird_server_reply:
        return "unexpected server reply";
      case transport_error::remote_access_denied:
        return "remote access denied";
      case transport_error::write_error:
        return "write error";
      case transport_error::read_error:
        return "read error";
      case transport_error::out_of_memory:
        return "out of memory";
      case transport_error::operation_timedout:
        return "operation timed out";
      case transport_error::ssl_connect_error:
        return "TLS connection error";
      case transport_error::file_couldnt_read_file:
        return "could not read file";
      case transport_error::aborted_by_callback:
        return "operation aborted by callback";
      case transport_error::bad_function_argument:
        return "bad transport argument";
      case transport_error::too_many_redirects:
        return "too many redirects";
      case transport_error::got_nothing:
        return "empty server response";
      case transport_error::send_error:
        return "send error";
      case transport_error::recv_error:
        return "receive error";
      case transport_error::ssl_certproblem:
        return "TLS certificate problem";
      case transport_error::ssl_cipher:
        return "TLS cipher error";
      case transport_error::peer_failed_verification:
        return "TLS peer verification failed";
      case transport_error::bad_content_encoding:
        return "bad content encoding";
      case transport_error::use_ssl_failed:
        return "failed to use TLS";
      case transport_error::ssl_cacert_badfile:
        return "bad CA certificate file";
      case transport_error::ssl_shutdown_failed:
        return "TLS shutdown failed";
      case transport_error::no_connection_available:
        return "no connection available";
      case transport_error::auth_error:
        return "authentication error";
      case transport_error::proxy:
        return "proxy error";
      case transport_error::too_large:
        return "payload too large";
      default:
        return "transport error " + std::to_string(code);
    }
  }
};

} // namespace

const std::error_category &transport_error_category() noexcept {
  static constant_init<transport_error_category_impl> transport_error_category_instance;
  return transport_error_category_instance.obj;
}

WUWE_NAMESPACE_END
