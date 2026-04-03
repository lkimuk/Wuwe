#include <wuwe/net/net_errc.h>

#include <wuwe/net/http_status_code.h>
#include <wuwe/net/transport_error.h>

#include <system_error>

WUWE_NAMESPACE_BEGIN

namespace {

template <typename T> struct constant_init {
  union {
    T obj;
  };
  constexpr constant_init() noexcept : obj() {}

  ~constant_init()
  { /* do nothing, union object is not destroyed */
  }
};

struct net_category_impl final : public std::error_category {
public:
  const char *name() const noexcept final
  {
    return "net";
  }

  bool equivalent(const std::error_code &ec, int condition) const noexcept final
  {
    using wuwe::http_status_code;
    using wuwe::net_errc;
    using wuwe::transport_error;

    const auto cond = static_cast<net_errc>(condition);

    switch (cond)
    {
      case net_errc::invalid_request:
        return ec == http_status_code::bad_request;
      case net_errc::unauthorized:
        return ec == http_status_code::unauthorized;
      case net_errc::forbidden:
        return ec == http_status_code::forbidden;
      case net_errc::not_found:
        return ec == http_status_code::not_found;
      case net_errc::method_not_allowed:
        return ec == http_status_code::method_not_allowed;
      case net_errc::conflict:
        return ec == http_status_code::conflict;
      case net_errc::rate_limited:
        return ec == http_status_code::too_many_requests;
      case net_errc::timeout:
        return ec == transport_error::operation_timedout || ec == http_status_code::gateway_timeout;
      case net_errc::name_resolution_failed:
        return ec == transport_error::couldnt_resolve_proxy ||
               ec == transport_error::couldnt_resolve_host;
      case net_errc::connection_failed:
        return ec == transport_error::couldnt_connect || ec == transport_error::send_error ||
               ec == transport_error::recv_error || ec == transport_error::got_nothing ||
               ec == transport_error::no_connection_available;
      case net_errc::tls_failed:
        return ec == transport_error::ssl_connect_error || ec == transport_error::ssl_certproblem ||
               ec == transport_error::ssl_cipher ||
               ec == transport_error::peer_failed_verification ||
               ec == transport_error::use_ssl_failed || ec == transport_error::ssl_cacert_badfile ||
               ec == transport_error::ssl_shutdown_failed ||
               ec == transport_error::ssl_crl_badfile || ec == transport_error::ssl_issuer_error ||
               ec == transport_error::ssl_pinnedpubkeynotmatch ||
               ec == transport_error::ssl_invalidcertstatus ||
               ec == transport_error::ssl_clientcert;
      case net_errc::transport_failed:
        return ec.category() == transport_error_category();
      case net_errc::server_error:
        return ec == http_status_code::internal_server_error ||
               ec == http_status_code::bad_gateway || ec == http_status_code::gateway_timeout;
      case net_errc::service_unavailable:
        return ec == http_status_code::service_unavailable;
      default:
        return false;
    }
  }

  std::string message(int code) const final
  {
    using wuwe::net_errc;
    switch (static_cast<net_errc>(code))
    {
      case net_errc::invalid_request:
        return "Invalid request";
      case net_errc::unauthorized:
        return "Unauthorized";
      case net_errc::forbidden:
        return "Forbidden";
      case net_errc::not_found:
        return "Not found";
      case net_errc::method_not_allowed:
        return "Method not allowed";
      case net_errc::conflict:
        return "Conflict";
      case net_errc::rate_limited:
        return "Rate limited";
      case net_errc::timeout:
        return "Timeout";
      case net_errc::name_resolution_failed:
        return "Name resolution failed";
      case net_errc::connection_failed:
        return "Connection failed";
      case net_errc::tls_failed:
        return "TLS failed";
      case net_errc::transport_failed:
        return "Transport failed";
      case net_errc::server_error:
        return "Server error";
      case net_errc::service_unavailable:
        return "Service unavailable";
      default:
        return "Unknown network error";
    }
  }
};

} // namespace

const std::error_category &net_category() noexcept
{
  static constant_init<net_category_impl> net_category_instance;
  return net_category_instance.obj;
}

WUWE_NAMESPACE_END
