#ifndef WUWE_NET_TRANSPORT_ERROR_H
#define WUWE_NET_TRANSPORT_ERROR_H

#include <cstdint>
#include <system_error>

#include <wuwe/common/wuwe_fwd.h>

WUWE_NAMESPACE_BEGIN

enum class transport_error : uint16_t {
  unsupported_protocol = 1,
  failed_init = 2,
  url_malformat = 3,
  not_built_in = 4,
  couldnt_resolve_proxy = 5,
  couldnt_resolve_host = 6,
  couldnt_connect = 7,
  weird_server_reply = 8,
  remote_access_denied = 9,
  http2 = 10,
  partial_file = 11,
  quote_error = 12,
  http_returned_error = 13,
  write_error = 14,
  upload_failed = 15,
  read_error = 16,
  out_of_memory = 17,
  operation_timedout = 18,
  range_error = 19,
  http_post_error = 20,
  ssl_connect_error = 21,
  bad_download_resume = 22,
  file_couldnt_read_file = 23,
  function_not_found = 24,
  aborted_by_callback = 25,
  bad_function_argument = 26,
  interface_failed = 27,
  too_many_redirects = 28,
  unknown_option = 29,
  setopt_option_syntax = 30,
  got_nothing = 31,
  ssl_engine_notfound = 32,
  ssl_engine_setfailed = 33,
  send_error = 34,
  recv_error = 35,
  ssl_certproblem = 36,
  ssl_cipher = 37,
  peer_failed_verification = 38,
  bad_content_encoding = 39,
  filesize_exceeded = 40,
  use_ssl_failed = 41,
  send_fail_rewind = 42,
  ssl_engine_initfailed = 43,
  login_denied = 44,
  ssl_cacert_badfile = 45,
  ssl_shutdown_failed = 46,
  again = 47,
  ssl_crl_badfile = 48,
  ssl_issuer_error = 49,
  chunk_failed = 50,
  no_connection_available = 51,
  ssl_pinnedpubkeynotmatch = 52,
  ssl_invalidcertstatus = 53,
  http2_stream = 54,
  recursive_api_call = 55,
  auth_error = 56,
  http3 = 57,
  quic_connect_error = 58,
  proxy = 59,
  ssl_clientcert = 60,
  unrecoverable_poll = 61,
  too_large = 62,

  unknown_error = 1000
};

inline constexpr uint16_t to_underlying(transport_error code) noexcept
{
  return static_cast<uint16_t>(code);
}

inline constexpr bool is_timeout(transport_error code) noexcept
{
  return code == transport_error::operation_timedout;
}

inline constexpr bool is_name_resolution_error(transport_error code) noexcept
{
  return code == transport_error::couldnt_resolve_proxy ||
         code == transport_error::couldnt_resolve_host;
}

inline constexpr bool is_connection_error(transport_error code) noexcept
{
  return code == transport_error::couldnt_connect || code == transport_error::send_error ||
         code == transport_error::recv_error || code == transport_error::got_nothing ||
         code == transport_error::no_connection_available;
}

inline constexpr bool is_tls_error(transport_error code) noexcept
{
  return code == transport_error::ssl_connect_error || code == transport_error::ssl_certproblem ||
         code == transport_error::ssl_cipher || code == transport_error::peer_failed_verification ||
         code == transport_error::use_ssl_failed || code == transport_error::ssl_cacert_badfile ||
         code == transport_error::ssl_shutdown_failed || code == transport_error::ssl_crl_badfile ||
         code == transport_error::ssl_issuer_error ||
         code == transport_error::ssl_pinnedpubkeynotmatch ||
         code == transport_error::ssl_invalidcertstatus || code == transport_error::ssl_clientcert;
}

[[nodiscard]] const ::std::error_category &transport_error_category() noexcept;

[[nodiscard]] inline std::error_code make_error_code(transport_error code) noexcept
{
  return {static_cast<int>(to_underlying(code)), transport_error_category()};
}

WUWE_NAMESPACE_END

template <> struct std::is_error_code_enum<wuwe::transport_error> : std::true_type {};

#endif // WUWE_NET_TRANSPORT_ERROR_H
