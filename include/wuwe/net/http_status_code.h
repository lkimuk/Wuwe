#ifndef WUWE_NET_HTTP_STATUS_CODE_H
#define WUWE_NET_HTTP_STATUS_CODE_H

#include <system_error>

#include <wuwe/common/wuwe_fwd.h>

WUWE_NAMESPACE_BEGIN

enum class http_status_code : int {
  continue_ = 100,
  switching_protocols = 101,

  ok = 200,
  created = 201,
  accepted = 202,
  no_content = 204,

  moved_permanently = 301,
  found = 302,
  not_modified = 304,

  bad_request = 400,
  unauthorized = 401,
  forbidden = 403,
  not_found = 404,
  method_not_allowed = 405,
  conflict = 409,
  too_many_requests = 429,

  internal_server_error = 500,
  bad_gateway = 502,
  service_unavailable = 503,
  gateway_timeout = 504
};

inline constexpr int to_underlying(http_status_code code) noexcept
{
  return static_cast<int>(code);
}

inline constexpr bool is_informational(http_status_code code) noexcept
{
  const int value = to_underlying(code);
  return value >= 100 && value < 200;
}

inline constexpr bool is_success(http_status_code code) noexcept
{
  const int value = to_underlying(code);
  return value >= 200 && value < 300;
}

inline constexpr bool is_redirection(http_status_code code) noexcept
{
  const int value = to_underlying(code);
  return value >= 300 && value < 400;
}

inline constexpr bool is_client_error(http_status_code code) noexcept
{
  const int value = to_underlying(code);
  return value >= 400 && value < 500;
}

inline constexpr bool is_server_error(http_status_code code) noexcept
{
  const int value = to_underlying(code);
  return value >= 500 && value < 600;
}

inline constexpr bool is_error(http_status_code code) noexcept
{
  return is_client_error(code) || is_server_error(code);
}

[[nodiscard]] const ::std::error_category &http_status_category() noexcept;

[[nodiscard]] inline std::error_code make_error_code(http_status_code code) noexcept
{
  return {is_success(code) ? 0 : to_underlying(code), http_status_category()};
}

WUWE_NAMESPACE_END

template <> struct std::is_error_code_enum<wuwe::http_status_code> : std::true_type {};

#endif // WUWE_NET_HTTP_STATUS_CODE_H
