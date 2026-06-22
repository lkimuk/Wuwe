#include <wuwe/agent/execution/path_policy.hpp>

#include <algorithm>
#include <cwctype>
#include <system_error>

namespace wuwe::agent::execution {
namespace {

std::filesystem::path weakly_canonical_noexcept(const std::filesystem::path& path) {
  std::error_code error;
  auto normalized = std::filesystem::weakly_canonical(path, error);
  if (!error) {
    return normalized.lexically_normal();
  }
  return std::filesystem::absolute(path, error).lexically_normal();
}

#ifdef _WIN32
std::wstring comparable_path_string(const std::filesystem::path& path) {
  auto text = path.wstring();
  std::replace(text.begin(), text.end(), L'/', L'\\');
  std::transform(text.begin(), text.end(), text.begin(), [](wchar_t ch) {
    return static_cast<wchar_t>(std::towlower(ch));
  });
  return text;
}
#else
std::string comparable_path_string(const std::filesystem::path& path) {
  return path.string();
}
#endif

} // namespace

std::filesystem::path normalize_existing_or_parent_path(
  const std::filesystem::path& path) {
  return weakly_canonical_noexcept(path);
}

bool path_is_within_root(
  const std::filesystem::path& path,
  const std::filesystem::path& root) {
  const auto normalized_path = normalize_existing_or_parent_path(path);
  const auto normalized_root = normalize_existing_or_parent_path(root);
  auto path_text = comparable_path_string(normalized_path);
  auto root_text = comparable_path_string(normalized_root);

  if (path_text == root_text) {
    return true;
  }
  if (root_text.empty()) {
    return false;
  }

#ifdef _WIN32
  if (root_text.back() != L'\\') {
    root_text.push_back(L'\\');
  }
#else
  if (root_text.back() != '/') {
    root_text.push_back('/');
  }
#endif
  return path_text.rfind(root_text, 0) == 0;
}

path_boundary_result evaluate_path_boundary(
  const std::filesystem::path& path,
  const std::vector<std::filesystem::path>& allowed_roots) {
  const auto normalized = normalize_existing_or_parent_path(path);
  for (const auto& root : allowed_roots) {
    if (path_is_within_root(normalized, root)) {
      return {
        .allowed = true,
        .normalized_path = normalized,
        .matched_root = normalize_existing_or_parent_path(root),
      };
    }
  }
  return {
    .allowed = false,
    .normalized_path = normalized,
    .error_message = "path is outside allowed roots",
  };
}

} // namespace wuwe::agent::execution
