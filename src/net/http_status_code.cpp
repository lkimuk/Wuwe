#include <wuwe/net/http_status_code.h>

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

struct http_status_category_impl final : public std::error_category {
public:
  const char *name() const noexcept final
  {
    return "http_status";
  }

  std::string message(int code) const final
  {
    using wuwe::http_status_code;
    switch (static_cast<http_status_code>(code))
    {
      case http_status_code::continue_:
        return "Continue";
      case http_status_code::switching_protocols:
        return "Switching Protocols";
      case http_status_code::ok:
        return "OK";
      case http_status_code::created:
        return "Created";
      case http_status_code::accepted:
        return "Accepted";
      case http_status_code::no_content:
        return "No Content";
      case http_status_code::moved_permanently:
        return "Moved Permanently";
      case http_status_code::found:
        return "Found";
      case http_status_code::not_modified:
        return "Not Modified";
      case http_status_code::bad_request:
        return "Bad Request";
      case http_status_code::unauthorized:
        return "Unauthorized";
      case http_status_code::forbidden:
        return "Forbidden";
      case http_status_code::not_found:
        return "Not Found";
      case http_status_code::method_not_allowed:
        return "Method Not Allowed";
      case http_status_code::conflict:
        return "Conflict";
      case http_status_code::too_many_requests:
        return "Too Many Requests";
      case http_status_code::internal_server_error:
        return "Internal Server Error";
      case http_status_code::bad_gateway:
        return "Bad Gateway";
      case http_status_code::service_unavailable:
        return "Service Unavailable";
      case http_status_code::gateway_timeout:
        return "Gateway Timeout";
      default:
        return "Unknown HTTP Status Code";
    }
  }
};

} // namespace

const std::error_category &http_status_category() noexcept
{
  static constant_init<http_status_category_impl> http_status_category_instantance;
  return http_status_category_instantance.obj;
}

WUWE_NAMESPACE_END
