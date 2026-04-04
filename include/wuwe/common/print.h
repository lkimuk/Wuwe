#ifndef WUWE_COMMON_PRINT_H
#define WUWE_COMMON_PRINT_H

#include <format>
#include <iostream>
#include <utility>

#include <gmp/macro/macro.hpp>

#if GMP_CPP_AT_LEAST(23)
#include <print>
#endif // C++23 or later

#include <wuwe/common/wuwe_fwd.h>

WUWE_NAMESPACE_BEGIN

template <typename... Args> inline void print(std::format_string<Args...> fmt, Args&&... args) {
#if GMP_CPP_NEWER_THAN(23)
  std::print(std::forward<std::format_string<Args...>>(fmt), std::forward<Args>(args)...);
#else
  std::cout << std::format(
    std::forward<std::format_string<Args...>>(fmt), std::forward<Args>(args)...);
#endif // C++23 or later
}

template <typename... Args> inline void println(std::format_string<Args...> fmt, Args&&... args) {
#if GMP_CPP_NEWER_THAN(23)
  std::println(std::forward<std::format_string<Args...>>(fmt), std::forward<Args>(args)...);
#else
  print(std::forward<std::format_string<Args...>>(fmt), std::forward<Args>(args)...);
  std::cout << '\n';
#endif // C++23 or later
}

WUWE_NAMESPACE_END

#endif // WUWE_COMMON_PRINT_H
