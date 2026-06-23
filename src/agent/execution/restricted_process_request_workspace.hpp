#ifndef WUWE_AGENT_EXECUTION_RESTRICTED_PROCESS_REQUEST_WORKSPACE_HPP
#define WUWE_AGENT_EXECUTION_RESTRICTED_PROCESS_REQUEST_WORKSPACE_HPP

#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

namespace wuwe::agent::execution::detail {

enum class restricted_request_workspace_status {
  ok,
  empty_root,
  empty_script_filename,
  invalid_script_filename,
  create_root_failed,
  create_request_directory_failed,
  write_script_failed,
};

struct restricted_request_workspace_request {
  std::filesystem::path root;
  std::string script_text;
  std::filesystem::path script_filename { "snippet.py" };
  std::string directory_prefix { "wuwe-restricted-request" };
  bool cleanup_on_destroy { true };
};

class restricted_request_workspace {
public:
  restricted_request_workspace() = default;
  ~restricted_request_workspace();

  restricted_request_workspace(const restricted_request_workspace&) = delete;
  restricted_request_workspace& operator=(const restricted_request_workspace&) =
    delete;

  restricted_request_workspace(restricted_request_workspace&& other) noexcept;
  restricted_request_workspace& operator=(
    restricted_request_workspace&& other) noexcept;

  [[nodiscard]] const std::filesystem::path& root() const noexcept {
    return root_;
  }

  [[nodiscard]] const std::filesystem::path& script_path() const noexcept {
    return script_path_;
  }

  [[nodiscard]] bool cleanup_on_destroy() const noexcept {
    return cleanup_on_destroy_;
  }

  void release_cleanup() noexcept {
    cleanup_on_destroy_ = false;
  }

private:
  friend struct restricted_request_workspace_result;
  friend restricted_request_workspace_result create_restricted_request_workspace(
    const restricted_request_workspace_request& request);

  void reset() noexcept;

  std::filesystem::path root_;
  std::filesystem::path script_path_;
  bool cleanup_on_destroy_ { true };
};

struct restricted_request_workspace_result {
  restricted_request_workspace_status status {
    restricted_request_workspace_status::ok
  };
  std::optional<restricted_request_workspace> workspace;
  std::error_code system_error;
  std::string detail;
};

[[nodiscard]] const char* to_string(
  restricted_request_workspace_status status) noexcept;

[[nodiscard]] restricted_request_workspace_result
create_restricted_request_workspace(
  const restricted_request_workspace_request& request);

} // namespace wuwe::agent::execution::detail

#endif // WUWE_AGENT_EXECUTION_RESTRICTED_PROCESS_REQUEST_WORKSPACE_HPP
