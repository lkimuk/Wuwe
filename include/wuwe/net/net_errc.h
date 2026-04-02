#ifndef WUWE_NET_NET_ERRC_H
#define WUWE_NET_NET_ERRC_H

#include <system_error>

#include <wuwe/common/wuwe_fwd.h>

WUWE_NAMESPACE_BEGIN

enum class net_errc : int {
    invalid_request,
    unauthorized,
    forbidden,
    not_found,
    method_not_allowed,
    conflict,
    rate_limited,
    timeout,
    name_resolution_failed,
    connection_failed,
    tls_failed,
    transport_failed,
    server_error,
    service_unavailable
};

[[nodiscard]]
const ::std::error_category& net_category() noexcept;

[[nodiscard]]
inline std::error_condition make_error_condition(net_errc code) noexcept {
    return { static_cast<int>(code), net_category() };
}

WUWE_NAMESPACE_END

template<>
struct std::is_error_condition_enum<wuwe::net_errc> : std::true_type {};

#endif // WUWE_NET_NET_ERRC_H
