#ifndef WUWE_AGENT_KNOWLEDGE_PATH_HPP
#define WUWE_AGENT_KNOWLEDGE_PATH_HPP

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace wuwe::agent::knowledge {

inline std::filesystem::path path_from_utf8(std::string_view value) {
#if defined(_WIN32)
  if (value.empty()) {
    return {};
  }

  const auto size = MultiByteToWideChar(
    CP_UTF8,
    MB_ERR_INVALID_CHARS,
    value.data(),
    static_cast<int>(value.size()),
    nullptr,
    0);
  if (size <= 0) {
    return std::filesystem::path(std::string(value));
  }

  std::wstring wide(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(
    CP_UTF8,
    MB_ERR_INVALID_CHARS,
    value.data(),
    static_cast<int>(value.size()),
    wide.data(),
    size);
  return std::filesystem::path(std::move(wide));
#else
  return std::filesystem::path(std::string(value));
#endif
}

inline std::string path_to_utf8(const std::filesystem::path& path) {
#if defined(_WIN32)
  const auto wide = path.wstring();
  if (wide.empty()) {
    return {};
  }

  const auto size = WideCharToMultiByte(
    CP_UTF8,
    0,
    wide.data(),
    static_cast<int>(wide.size()),
    nullptr,
    0,
    nullptr,
    nullptr);
  if (size <= 0) {
    return path.string();
  }

  std::string result(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(
    CP_UTF8,
    0,
    wide.data(),
    static_cast<int>(wide.size()),
    result.data(),
    size,
    nullptr,
    nullptr);
  return result;
#else
  const auto value = path.generic_u8string();
  return std::string(value.begin(), value.end());
#endif
}

inline std::string filename_to_utf8(const std::filesystem::path& path) {
  return path_to_utf8(path.filename());
}

inline std::string stem_to_utf8(const std::filesystem::path& path) {
  return path_to_utf8(path.stem());
}

inline std::string generic_path_to_utf8(const std::filesystem::path& path) {
  auto value = path_to_utf8(path);
  for (auto& ch : value) {
    if (ch == '\\') {
      ch = '/';
    }
  }
  return value;
}

} // namespace wuwe::agent::knowledge

#endif // WUWE_AGENT_KNOWLEDGE_PATH_HPP
