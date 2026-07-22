#ifndef WUWE_EXAMPLES_CONSOLE_UTF8_HPP
#define WUWE_EXAMPLES_CONSOLE_UTF8_HPP

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace wuwe_example {

inline void configure_utf8_console() noexcept {
#ifdef _WIN32
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
#endif
}

} // namespace wuwe_example

#endif // WUWE_EXAMPLES_CONSOLE_UTF8_HPP
