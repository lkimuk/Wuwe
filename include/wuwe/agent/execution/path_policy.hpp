#ifndef WUWE_AGENT_EXECUTION_PATH_POLICY_HPP
#define WUWE_AGENT_EXECUTION_PATH_POLICY_HPP

#include <filesystem>
#include <string>
#include <vector>

namespace wuwe::agent::execution {

struct path_boundary_result {
  bool allowed { false };
  std::filesystem::path normalized_path;
  std::filesystem::path matched_root;
  std::string error_message;
};

[[nodiscard]] std::filesystem::path normalize_existing_or_parent_path(
  const std::filesystem::path& path);

[[nodiscard]] bool path_is_within_root(
  const std::filesystem::path& path,
  const std::filesystem::path& root);

[[nodiscard]] path_boundary_result evaluate_path_boundary(
  const std::filesystem::path& path,
  const std::vector<std::filesystem::path>& allowed_roots);

} // namespace wuwe::agent::execution

#endif // WUWE_AGENT_EXECUTION_PATH_POLICY_HPP
