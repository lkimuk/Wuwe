#ifndef WUWE_AGENT_EXECUTION_RESTRICTED_PROCESS_APPCONTAINER_LAUNCH_WIN32_HPP
#define WUWE_AGENT_EXECUTION_RESTRICTED_PROCESS_APPCONTAINER_LAUNCH_WIN32_HPP

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <filesystem>
#include <map>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace wuwe::agent::execution::detail {

enum class restricted_appcontainer_launch_status {
  ok,
  invalid_appcontainer_sid,
  create_pipe_failed,
  set_handle_information_failed,
  attribute_list_failed,
  create_job_failed,
  configure_job_failed,
  launch_failed,
  assign_job_failed,
  resume_failed,
  wait_failed,
  get_exit_code_failed,
};

struct restricted_appcontainer_process_capture {
  DWORD exit_code { 1 };
  bool timed_out { false };
  bool cancelled { false };
  bool stdout_truncated { false };
  bool stderr_truncated { false };
  std::string stdout_text;
  std::string stderr_text;
};

struct restricted_appcontainer_launch_request {
  std::filesystem::path executable;
  PSID appcontainer_sid {};
  std::vector<std::wstring> arguments;
  std::filesystem::path working_directory;
  std::string stdin_text;
  std::chrono::milliseconds timeout { 5000 };
  std::size_t max_stdout_bytes { 65536 };
  std::size_t max_stderr_bytes { 65536 };
  std::optional<std::map<std::wstring, std::wstring>> environment;
  std::stop_token stop_token;
};

struct restricted_appcontainer_launch_result {
  restricted_appcontainer_launch_status status {
    restricted_appcontainer_launch_status::ok
  };
  restricted_appcontainer_process_capture capture;
  DWORD win32_error { ERROR_SUCCESS };
  std::string detail;
};

[[nodiscard]] const char* to_string(
  restricted_appcontainer_launch_status status) noexcept;

[[nodiscard]] restricted_appcontainer_launch_result
launch_restricted_appcontainer_process(
  restricted_appcontainer_launch_request request);

} // namespace wuwe::agent::execution::detail

#endif // _WIN32

#endif // WUWE_AGENT_EXECUTION_RESTRICTED_PROCESS_APPCONTAINER_LAUNCH_WIN32_HPP
