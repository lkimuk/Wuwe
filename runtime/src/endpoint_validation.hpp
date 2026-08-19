#ifndef WUWE_RUNTIME_HOST_ENDPOINT_VALIDATION_HPP
#define WUWE_RUNTIME_HOST_ENDPOINT_VALIDATION_HPP

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <string_view>
#include <system_error>

namespace wuwe::runtime_host::detail {

inline bool valid_port(std::string_view value) noexcept {
  if (value.empty())
    return false;
  unsigned int port = 0;
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), port);
  return error == std::errc {} && end == value.data() + value.size() && port > 0 && port <= 65535;
}

inline bool valid_ipv4_address(std::string_view value) noexcept {
  std::size_t begin = 0;
  int components = 0;
  while (begin <= value.size()) {
    const auto end = value.find('.', begin);
    const auto component =
      value.substr(begin, end == value.npos ? value.size() - begin : end - begin);
    if (component.empty() || component.size() > 3)
      return false;
    unsigned int octet = 0;
    const auto [parsed, error] =
      std::from_chars(component.data(), component.data() + component.size(), octet);
    if (error != std::errc {} || parsed != component.data() + component.size() || octet > 255)
      return false;
    ++components;
    if (end == value.npos)
      break;
    begin = end + 1;
  }
  return components == 4;
}

inline bool valid_dns_name(std::string_view name, bool lowercase_only = false) noexcept {
  if (name.empty() || name.size() > 253)
    return false;
  std::size_t begin = 0;
  while (begin <= name.size()) {
    const auto end = name.find('.', begin);
    const auto label = name.substr(begin, end == name.npos ? name.size() - begin : end - begin);
    if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-')
      return false;
    if (!std::all_of(label.begin(), label.end(), [lowercase_only](unsigned char value) {
          const bool lowercase = value >= 'a' && value <= 'z';
          const bool uppercase = value >= 'A' && value <= 'Z';
          const bool digit = value >= '0' && value <= '9';
          return lowercase || (!lowercase_only && uppercase) || digit || value == '-';
        })) {
      return false;
    }
    if (end == name.npos)
      return true;
    begin = end + 1;
  }
  return false;
}

inline bool valid_ipv6_side(std::string_view side, bool allow_ipv4_tail, int& groups) noexcept {
  if (side.empty())
    return true;
  std::size_t begin = 0;
  while (begin <= side.size()) {
    const auto end = side.find(':', begin);
    const auto component =
      side.substr(begin, end == side.npos ? side.size() - begin : end - begin);
    if (component.empty())
      return false;
    if (component.find('.') != component.npos) {
      if (!allow_ipv4_tail || end != side.npos || !valid_ipv4_address(component))
        return false;
      groups += 2;
    }
    else {
      if (component.size() > 4 || !std::all_of(component.begin(), component.end(), [](char value) {
            return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
                   (value >= 'A' && value <= 'F');
          })) {
        return false;
      }
      ++groups;
    }
    if (end == side.npos)
      return true;
    begin = end + 1;
  }
  return false;
}

inline bool valid_ipv6_address(std::string_view value) noexcept {
  if (value.empty())
    return false;
  const auto compression = value.find("::");
  if (compression != value.npos && value.find("::", compression + 2) != value.npos)
    return false;
  int groups = 0;
  if (compression == value.npos)
    return valid_ipv6_side(value, true, groups) && groups == 8;
  const auto left = value.substr(0, compression);
  const auto right = value.substr(compression + 2);
  if (!valid_ipv6_side(left, false, groups) || !valid_ipv6_side(right, true, groups))
    return false;
  return groups < 8;
}

inline bool valid_host(std::string_view value, bool lowercase_only = false) noexcept {
  if (value.empty())
    return false;
  const bool numeric_address = std::all_of(value.begin(), value.end(), [](char character) {
    return (character >= '0' && character <= '9') || character == '.';
  });
  return numeric_address ? valid_ipv4_address(value) : valid_dns_name(value, lowercase_only);
}

inline bool valid_network_endpoint(std::string_view endpoint) noexcept {
  if (endpoint.empty())
    return false;
  if (endpoint.front() == '[') {
    const auto close = endpoint.find(']');
    if (close == endpoint.npos || !valid_ipv6_address(endpoint.substr(1, close - 1)))
      return false;
    const auto suffix = endpoint.substr(close + 1);
    return suffix.empty() || (suffix.front() == ':' && valid_port(suffix.substr(1)));
  }
  const auto separator = endpoint.rfind(':');
  if (separator != endpoint.npos && endpoint.find(':') != separator)
    return false;
  const auto host = separator == endpoint.npos ? endpoint : endpoint.substr(0, separator);
  return valid_host(host) &&
         (separator == endpoint.npos || valid_port(endpoint.substr(separator + 1)));
}

} // namespace wuwe::runtime_host::detail

#endif // WUWE_RUNTIME_HOST_ENDPOINT_VALIDATION_HPP
