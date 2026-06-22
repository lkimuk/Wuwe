#include <wuwe/agent/execution/controlled_process_backend.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <functional>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace wuwe::agent::execution {
namespace {

std::string stable_error_code_for_status(python_interpreter_status status) {
  switch (status) {
    case python_interpreter_status::ok:
      return "ok";
    case python_interpreter_status::empty_path:
      return "python_interpreter_empty_path";
    case python_interpreter_status::not_found:
      return "python_interpreter_not_found";
    case python_interpreter_status::not_executable:
      return "python_interpreter_not_executable";
    case python_interpreter_status::permission_denied:
      return "python_interpreter_permission_denied";
    case python_interpreter_status::startup_timeout:
      return "python_interpreter_startup_timeout";
    case python_interpreter_status::invalid_python:
      return "python_interpreter_invalid";
    case python_interpreter_status::unsupported_version:
      return "python_interpreter_unsupported_version";
    case python_interpreter_status::launch_failed:
    default:
      return "python_interpreter_launch_failed";
  }
}

#ifdef _WIN32

class unique_handle {
public:
  unique_handle() = default;
  explicit unique_handle(HANDLE handle) noexcept : handle_(handle) {
  }

  ~unique_handle() {
    reset();
  }

  unique_handle(const unique_handle&) = delete;
  unique_handle& operator=(const unique_handle&) = delete;

  unique_handle(unique_handle&& other) noexcept : handle_(other.release()) {
  }

  unique_handle& operator=(unique_handle&& other) noexcept {
    if (this != &other) {
      reset(other.release());
    }
    return *this;
  }

  [[nodiscard]] HANDLE get() const noexcept {
    return handle_;
  }

  [[nodiscard]] bool valid() const noexcept {
    return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
  }

  [[nodiscard]] HANDLE release() noexcept {
    const auto handle = handle_;
    handle_ = nullptr;
    return handle;
  }

  void reset(HANDLE handle = nullptr) noexcept {
    if (valid()) {
      CloseHandle(handle_);
    }
    handle_ = handle;
  }

private:
  HANDLE handle_ { nullptr };
};

std::error_code win32_error_code(DWORD error) {
  return {
    static_cast<int>(error),
    std::system_category(),
  };
}

std::string win32_error_message(DWORD error) {
  if (error == ERROR_SUCCESS) {
    return {};
  }

  LPSTR raw_message = nullptr;
  const auto flags =
    FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
    FORMAT_MESSAGE_IGNORE_INSERTS;
  const auto size = FormatMessageA(
    flags,
    nullptr,
    error,
    MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
    reinterpret_cast<LPSTR>(&raw_message),
    0,
    nullptr);
  if (size == 0 || raw_message == nullptr) {
    return "Win32 error " + std::to_string(error);
  }

  struct message_cleanup {
    LPSTR message;
    ~message_cleanup() {
      if (message != nullptr) {
        LocalFree(message);
      }
    }
  } cleanup { raw_message };

  std::string message(raw_message, size);
  while (!message.empty() &&
         (message.back() == '\n' || message.back() == '\r' ||
          message.back() == ' ')) {
    message.pop_back();
  }
  return message;
}

python_interpreter_status status_for_win32_launch_error(DWORD error) {
  switch (error) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
      return python_interpreter_status::not_found;
    case ERROR_ACCESS_DENIED:
    case ERROR_ELEVATION_REQUIRED:
      return python_interpreter_status::permission_denied;
    case ERROR_BAD_EXE_FORMAT:
    case ERROR_EXE_MACHINE_TYPE_MISMATCH:
      return python_interpreter_status::not_executable;
    default:
      return python_interpreter_status::launch_failed;
  }
}

execution_result last_error_result(
  execution_termination_reason reason,
  std::string message) {
  const auto error = GetLastError();
  if (error != ERROR_SUCCESS) {
    message += " (win32_error=" + std::to_string(error) + ")";
  }
  return {
    .termination_reason = reason,
    .error_message = std::move(message),
  };
}

void add_python_launch_metadata(
  execution_result& result,
  const std::filesystem::path& interpreter,
  python_interpreter_status status,
  DWORD launch_error,
  std::string timeout_phase = {}) {
  result.metadata["error_code"] = stable_error_code_for_status(status);
  result.metadata["python_interpreter"] = interpreter.string();
  if (launch_error != ERROR_SUCCESS) {
    result.metadata["launch_error_code"] = std::to_string(launch_error);
    result.metadata["launch_error_message"] = win32_error_message(launch_error);
  }
  if (!timeout_phase.empty()) {
    result.metadata["timeout_phase"] = std::move(timeout_phase);
  }
}

execution_result python_launch_failure_result(
  const std::filesystem::path& interpreter,
  python_interpreter_status status,
  DWORD launch_error,
  std::string message) {
  execution_result result {
    .termination_reason = execution_termination_reason::launch_failed,
    .error_message = std::move(message),
  };
  add_python_launch_metadata(result, interpreter, status, launch_error);
  result.metadata["timeout_phase"] = "launch";
  return result;
}

void add_job_metadata(
  execution_result& result,
  const controlled_process_backend_config& config,
  const execution_request& request) {
  result.metadata["job_object_enabled"] = config.use_job_object ? "true" : "false";
  result.metadata["process_tree_cleanup_enforcement"] =
    config.use_job_object ? "enforced" : "not_enforced";
  result.metadata["process_count_limit_enforcement"] =
    config.use_job_object ? "enforced" : "not_enforced";
  result.metadata["cpu_time_limit_enforcement"] =
    config.use_job_object ? "enforced" : "not_enforced";
  result.metadata["memory_limit_enforcement"] =
    config.use_job_object ? "enforced" : "not_enforced";
  result.metadata["max_process_count"] =
    std::to_string(request.limits.max_process_count);
  result.metadata["max_memory_bytes"] =
    std::to_string(request.limits.max_memory_bytes);
  result.metadata["max_cpu_time_ms"] =
    std::to_string(request.limits.max_cpu_time.count());
}

bool configure_job_limits(
  HANDLE job,
  const execution_request& request,
  std::string& error_message) {
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

  if (request.limits.max_process_count > 0) {
    limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
    limits.BasicLimitInformation.ActiveProcessLimit =
      static_cast<DWORD>(request.limits.max_process_count);
  }
  if (request.limits.max_memory_bytes > 0) {
    limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_JOB_MEMORY;
    limits.JobMemoryLimit = static_cast<SIZE_T>(request.limits.max_memory_bytes);
  }
  if (request.limits.max_cpu_time.count() > 0) {
    limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_JOB_TIME;
    limits.BasicLimitInformation.PerJobUserTimeLimit.QuadPart =
      request.limits.max_cpu_time.count() * 10000LL;
  }

  if (!SetInformationJobObject(
        job,
        JobObjectExtendedLimitInformation,
        &limits,
        sizeof(limits))) {
    error_message = "failed to configure job object limits";
    return false;
  }
  return true;
}

class process_thread_attribute_list {
public:
  process_thread_attribute_list() = default;

  ~process_thread_attribute_list() {
    if (list_ != nullptr) {
      DeleteProcThreadAttributeList(list_);
    }
  }

  process_thread_attribute_list(const process_thread_attribute_list&) = delete;
  process_thread_attribute_list& operator=(const process_thread_attribute_list&) = delete;

  [[nodiscard]] bool initialize_with_handle_list(
    HANDLE* handles,
    DWORD handle_count) {
    SIZE_T attribute_list_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_list_size);
    if (attribute_list_size == 0) {
      return false;
    }

    storage_.resize(attribute_list_size);
    list_ = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data());
    if (!InitializeProcThreadAttributeList(list_, 1, 0, &attribute_list_size)) {
      list_ = nullptr;
      storage_.clear();
      return false;
    }

    if (!UpdateProcThreadAttribute(
          list_,
          0,
          PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
          handles,
          sizeof(HANDLE) * handle_count,
          nullptr,
          nullptr)) {
      DeleteProcThreadAttributeList(list_);
      list_ = nullptr;
      storage_.clear();
      return false;
    }
    return true;
  }

  [[nodiscard]] LPPROC_THREAD_ATTRIBUTE_LIST get() const noexcept {
    return list_;
  }

private:
  std::vector<char> storage_;
  LPPROC_THREAD_ATTRIBUTE_LIST list_ { nullptr };
};

std::wstring quote_windows_arg(std::wstring arg) {
  if (arg.empty()) {
    return L"\"\"";
  }

  bool needs_quotes = false;
  for (const auto ch : arg) {
    if (ch == L' ' || ch == L'\t' || ch == L'"') {
      needs_quotes = true;
      break;
    }
  }
  if (!needs_quotes) {
    return arg;
  }

  std::wstring quoted = L"\"";
  std::size_t backslashes = 0;
  for (const auto ch : arg) {
    if (ch == L'\\') {
      ++backslashes;
      continue;
    }
    if (ch == L'"') {
      quoted.append(backslashes * 2 + 1, L'\\');
      quoted.push_back(ch);
      backslashes = 0;
      continue;
    }
    quoted.append(backslashes, L'\\');
    backslashes = 0;
    quoted.push_back(ch);
  }
  quoted.append(backslashes * 2, L'\\');
  quoted.push_back(L'"');
  return quoted;
}

std::optional<std::wstring> utf8_to_wide(const std::string& text) {
  if (text.empty()) {
    return std::wstring {};
  }

  const auto required = MultiByteToWideChar(
    CP_UTF8,
    MB_ERR_INVALID_CHARS,
    text.data(),
    static_cast<int>(text.size()),
    nullptr,
    0);
  if (required <= 0) {
    return std::nullopt;
  }

  std::wstring output(static_cast<std::size_t>(required), L'\0');
  const auto written = MultiByteToWideChar(
    CP_UTF8,
    MB_ERR_INVALID_CHARS,
    text.data(),
    static_cast<int>(text.size()),
    output.data(),
    required);
  if (written != required) {
    return std::nullopt;
  }
  return output;
}

struct environment_block {
  std::vector<wchar_t> data;
  std::string error_message;
};

environment_block make_environment_block(
  const std::map<std::string, std::string>& env) {
  std::wstring block;
  for (const auto& [key, value] : env) {
    const auto wide_key = utf8_to_wide(key);
    const auto wide_value = utf8_to_wide(value);
    if (!wide_key.has_value() || !wide_value.has_value()) {
      return {
        .error_message =
          "environment contains a value that is not valid UTF-8: " + key,
      };
    }

    std::wstring entry = *wide_key;
    entry.push_back(L'=');
    entry.append(*wide_value);
    block.append(entry);
    block.push_back(L'\0');
  }
  block.push_back(L'\0');
  if (env.empty()) {
    block.push_back(L'\0');
  }
  return {
    .data = { block.begin(), block.end() },
  };
}

std::filesystem::path choose_workdir(
  const controlled_process_backend_config& config,
  const execution_request& request) {
  if (!request.workdir.empty()) {
    return request.workdir;
  }
  if (!config.fallback_workdir.empty()) {
    return config.fallback_workdir;
  }
  return std::filesystem::temp_directory_path() / "wuwe-execution";
}

std::filesystem::path script_path_for(
  const std::filesystem::path& workdir,
  const execution_request& request) {
  static std::atomic_uint64_t next_script_id { 1 };

  const auto raw_execution_id = request.metadata.contains("execution_id")
                                  ? request.metadata.at("execution_id")
                                  : std::string("script");
  std::string execution_id;
  execution_id.reserve(raw_execution_id.size());
  for (const auto ch : raw_execution_id) {
    const auto value = static_cast<unsigned char>(ch);
    if (std::isalnum(value) != 0 || ch == '-' || ch == '_' || ch == '.') {
      execution_id.push_back(ch);
    }
    else {
      execution_id.push_back('_');
    }
  }
  if (execution_id.empty()) {
    execution_id = "script";
  }

  return workdir / ("wuwe-" + std::to_string(GetCurrentProcessId()) + "-" +
                    std::to_string(next_script_id.fetch_add(1)) + "-" +
                    execution_id + ".py");
}

void read_pipe_to_string(
  HANDLE pipe,
  std::string& output,
  std::size_t limit,
  bool& truncated) {
  unique_handle handle(pipe);
  std::array<char, 4096> buffer {};
  for (;;) {
    DWORD bytes_read = 0;
    const auto ok = ReadFile(
      handle.get(),
      buffer.data(),
      static_cast<DWORD>(buffer.size()),
      &bytes_read,
      nullptr);
    if (!ok || bytes_read == 0) {
      break;
    }

    const auto remaining = limit > output.size() ? limit - output.size() : 0;
    const auto to_append =
      std::min<std::size_t>(remaining, static_cast<std::size_t>(bytes_read));
    if (to_append > 0) {
      output.append(buffer.data(), to_append);
    }
    if (to_append < static_cast<std::size_t>(bytes_read)) {
      truncated = true;
    }
  }
}

void write_string_to_pipe(HANDLE pipe, const std::string& input) {
  unique_handle handle(pipe);
  std::size_t offset = 0;
  while (offset < input.size()) {
    const auto chunk = std::min<std::size_t>(input.size() - offset, 4096);
    DWORD bytes_written = 0;
    const auto ok = WriteFile(
      handle.get(),
      input.data() + offset,
      static_cast<DWORD>(chunk),
      &bytes_written,
      nullptr);
    if (!ok || bytes_written == 0) {
      break;
    }
    offset += bytes_written;
  }
}

struct process_capture {
  bool created { false };
  bool timed_out { false };
  std::optional<int> exit_code;
  std::string stdout_text;
  std::string stderr_text;
  std::chrono::milliseconds elapsed { 0 };
  DWORD launch_error { ERROR_SUCCESS };
};

process_capture run_python_script_file(
  const std::filesystem::path& interpreter,
  const std::filesystem::path& script_path,
  const std::filesystem::path& workdir,
  const std::map<std::string, std::string>& env,
  std::chrono::milliseconds timeout,
  std::size_t max_stdout_bytes,
  std::size_t max_stderr_bytes,
  std::stop_token stop_token = {}) {
  process_capture capture;

  SECURITY_ATTRIBUTES security_attributes {
    .nLength = sizeof(SECURITY_ATTRIBUTES),
    .lpSecurityDescriptor = nullptr,
    .bInheritHandle = TRUE,
  };

  unique_handle stdin_read;
  unique_handle stdin_write;
  unique_handle stdout_read;
  unique_handle stdout_write;
  unique_handle stderr_read;
  unique_handle stderr_write;

  HANDLE raw_stdin_read = nullptr;
  HANDLE raw_stdin_write = nullptr;
  HANDLE raw_stdout_read = nullptr;
  HANDLE raw_stdout_write = nullptr;
  HANDLE raw_stderr_read = nullptr;
  HANDLE raw_stderr_write = nullptr;

  if (!CreatePipe(&raw_stdin_read, &raw_stdin_write, &security_attributes, 0)) {
    capture.launch_error = GetLastError();
    return capture;
  }
  stdin_read.reset(raw_stdin_read);
  stdin_write.reset(raw_stdin_write);

  if (!CreatePipe(&raw_stdout_read, &raw_stdout_write, &security_attributes, 0)) {
    capture.launch_error = GetLastError();
    return capture;
  }
  stdout_read.reset(raw_stdout_read);
  stdout_write.reset(raw_stdout_write);

  if (!CreatePipe(&raw_stderr_read, &raw_stderr_write, &security_attributes, 0)) {
    capture.launch_error = GetLastError();
    return capture;
  }
  stderr_read.reset(raw_stderr_read);
  stderr_write.reset(raw_stderr_write);

  if (!SetHandleInformation(stdin_write.get(), HANDLE_FLAG_INHERIT, 0) ||
      !SetHandleInformation(stdout_read.get(), HANDLE_FLAG_INHERIT, 0) ||
      !SetHandleInformation(stderr_read.get(), HANDLE_FLAG_INHERIT, 0)) {
    capture.launch_error = GetLastError();
    return capture;
  }

  STARTUPINFOW startup {};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = stdin_read.get();
  startup.hStdOutput = stdout_write.get();
  startup.hStdError = stderr_write.get();

  STARTUPINFOEXW startup_ex {};
  startup_ex.StartupInfo = startup;
  startup_ex.StartupInfo.cb = sizeof(startup_ex);
  HANDLE inherited_handles[] {
    stdin_read.get(),
    stdout_write.get(),
    stderr_write.get(),
  };
  process_thread_attribute_list attribute_list;
  if (!attribute_list.initialize_with_handle_list(
        inherited_handles,
        static_cast<DWORD>(std::size(inherited_handles)))) {
    capture.launch_error = GetLastError();
    if (capture.launch_error == ERROR_SUCCESS) {
      capture.launch_error = ERROR_INVALID_PARAMETER;
    }
    return capture;
  }
  startup_ex.lpAttributeList = attribute_list.get();

  auto environment = make_environment_block(env);
  if (!environment.error_message.empty()) {
    capture.launch_error = ERROR_INVALID_PARAMETER;
    capture.stderr_text = environment.error_message;
    return capture;
  }

  PROCESS_INFORMATION process {};
  auto command_line =
    quote_windows_arg(interpreter.wstring()) + L" " +
    quote_windows_arg(script_path.wstring());
  auto workdir_w = workdir.wstring();
  const auto created = CreateProcessW(
    nullptr,
    command_line.data(),
    nullptr,
    nullptr,
    TRUE,
    CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT |
      EXTENDED_STARTUPINFO_PRESENT,
    environment.data.data(),
    workdir_w.empty() ? nullptr : workdir_w.c_str(),
    &startup_ex.StartupInfo,
    &process);

  stdin_read.reset();
  stdout_write.reset();
  stderr_write.reset();

  if (!created) {
    capture.launch_error = GetLastError();
    return capture;
  }
  capture.created = true;

  unique_handle process_handle(process.hProcess);
  unique_handle thread_handle(process.hThread);
  stdin_write.reset();

  bool stdout_truncated = false;
  bool stderr_truncated = false;
  std::thread stdout_thread(
    read_pipe_to_string,
    stdout_read.release(),
    std::ref(capture.stdout_text),
    max_stdout_bytes,
    std::ref(stdout_truncated));
  std::thread stderr_thread(
    read_pipe_to_string,
    stderr_read.release(),
    std::ref(capture.stderr_text),
    max_stderr_bytes,
    std::ref(stderr_truncated));

  const auto started = std::chrono::steady_clock::now();
  for (;;) {
    const auto wait_result = WaitForSingleObject(process_handle.get(), 50);
    if (wait_result == WAIT_OBJECT_0) {
      break;
    }
    if (stop_token.stop_requested() ||
        (timeout.count() > 0 &&
         std::chrono::steady_clock::now() - started >= timeout)) {
      capture.timed_out = !stop_token.stop_requested();
      TerminateProcess(process_handle.get(), 1);
      WaitForSingleObject(process_handle.get(), INFINITE);
      break;
    }
  }

  if (stdout_thread.joinable()) {
    stdout_thread.join();
  }
  if (stderr_thread.joinable()) {
    stderr_thread.join();
  }
  capture.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now() - started);

  DWORD exit_code = 1;
  if (GetExitCodeProcess(process_handle.get(), &exit_code)) {
    capture.exit_code = static_cast<int>(exit_code);
  }
  return capture;
}

std::filesystem::path probe_script_path_for(const std::filesystem::path& workdir) {
  static std::atomic_uint64_t next_probe_id { 1 };
  return workdir / ("wuwe-python-probe-" +
                    std::to_string(GetCurrentProcessId()) + "-" +
                    std::to_string(next_probe_id.fetch_add(1)) + ".py");
}

execution_result launch_python_process(
  const controlled_process_backend_config& config,
  const execution_request& request,
  std::stop_token stop_token) {
  if (request.use_shell) {
    return {
      .termination_reason = execution_termination_reason::policy_denied,
      .error_message = "controlled process backend does not support shell execution",
    };
  }

  const auto workdir = choose_workdir(config, request);
  const auto script_path = script_path_for(workdir, request);

  try {
    std::filesystem::create_directories(workdir);
    std::ofstream script(script_path, std::ios::binary);
    if (!script) {
      return {
        .termination_reason = execution_termination_reason::launch_failed,
        .error_message = "failed to create Python snippet file: " + script_path.string(),
      };
    }
    script << request.code;
  }
  catch (const std::exception& ex) {
    return {
      .termination_reason = execution_termination_reason::launch_failed,
      .error_message = ex.what(),
    };
  }

  struct script_cleanup {
    std::filesystem::path path;
    ~script_cleanup() {
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
    }
  } cleanup { script_path };

  SECURITY_ATTRIBUTES security_attributes {
    .nLength = sizeof(SECURITY_ATTRIBUTES),
    .lpSecurityDescriptor = nullptr,
    .bInheritHandle = TRUE,
  };

  unique_handle stdin_read;
  unique_handle stdin_write;
  unique_handle stdout_read;
  unique_handle stdout_write;
  unique_handle stderr_read;
  unique_handle stderr_write;

  HANDLE raw_stdin_read = nullptr;
  HANDLE raw_stdin_write = nullptr;
  HANDLE raw_stdout_read = nullptr;
  HANDLE raw_stdout_write = nullptr;
  HANDLE raw_stderr_read = nullptr;
  HANDLE raw_stderr_write = nullptr;

  if (!CreatePipe(&raw_stdin_read, &raw_stdin_write, &security_attributes, 0)) {
    return {
      .termination_reason = execution_termination_reason::launch_failed,
      .error_message = "failed to create process pipes",
    };
  }
  stdin_read.reset(raw_stdin_read);
  stdin_write.reset(raw_stdin_write);

  if (!CreatePipe(&raw_stdout_read, &raw_stdout_write, &security_attributes, 0)) {
    return {
      .termination_reason = execution_termination_reason::launch_failed,
      .error_message = "failed to create process pipes",
    };
  }
  stdout_read.reset(raw_stdout_read);
  stdout_write.reset(raw_stdout_write);

  if (!CreatePipe(&raw_stderr_read, &raw_stderr_write, &security_attributes, 0)) {
    return {
      .termination_reason = execution_termination_reason::launch_failed,
      .error_message = "failed to create process pipes",
    };
  }
  stderr_read.reset(raw_stderr_read);
  stderr_write.reset(raw_stderr_write);

  if (!SetHandleInformation(stdin_write.get(), HANDLE_FLAG_INHERIT, 0) ||
      !SetHandleInformation(stdout_read.get(), HANDLE_FLAG_INHERIT, 0) ||
      !SetHandleInformation(stderr_read.get(), HANDLE_FLAG_INHERIT, 0)) {
    return {
      .termination_reason = execution_termination_reason::launch_failed,
      .error_message = "failed to configure process pipe inheritance",
    };
  }

  STARTUPINFOW startup {};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = stdin_read.get();
  startup.hStdOutput = stdout_write.get();
  startup.hStdError = stderr_write.get();

  STARTUPINFOEXW startup_ex {};
  startup_ex.StartupInfo = startup;
  startup_ex.StartupInfo.cb = sizeof(startup_ex);
  HANDLE inherited_handles[] {
    stdin_read.get(),
    stdout_write.get(),
    stderr_write.get(),
  };
  process_thread_attribute_list attribute_list;
  if (!attribute_list.initialize_with_handle_list(
        inherited_handles,
        static_cast<DWORD>(std::size(inherited_handles)))) {
    return {
      .termination_reason = execution_termination_reason::launch_failed,
      .error_message = "failed to restrict process handle inheritance",
    };
  }
  startup_ex.lpAttributeList = attribute_list.get();

  PROCESS_INFORMATION process {};
  const auto interpreter = config.python_interpreter.wstring();
  auto command_line =
    quote_windows_arg(interpreter) + L" " + quote_windows_arg(script_path.wstring());
  auto environment = make_environment_block(request.env);
  if (!environment.error_message.empty()) {
    return {
      .termination_reason = execution_termination_reason::launch_failed,
      .error_message = environment.error_message,
    };
  }
  auto workdir_w = workdir.wstring();
  unique_handle job_handle;
  DWORD creation_flags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT |
                         EXTENDED_STARTUPINFO_PRESENT;
  if (config.use_job_object) {
    job_handle.reset(CreateJobObjectW(nullptr, nullptr));
    if (!job_handle.valid()) {
      return last_error_result(
        execution_termination_reason::launch_failed,
        "failed to create job object");
    }
    std::string job_error;
    if (!configure_job_limits(job_handle.get(), request, job_error)) {
      return last_error_result(
        execution_termination_reason::launch_failed,
        job_error);
    }
    creation_flags |= CREATE_SUSPENDED;
  }

  const auto created = CreateProcessW(
    nullptr,
    command_line.data(),
    nullptr,
    nullptr,
    TRUE,
    creation_flags,
    environment.data.data(),
    workdir_w.empty() ? nullptr : workdir_w.c_str(),
    &startup_ex.StartupInfo,
    &process);

  stdin_read.reset();
  stdout_write.reset();
  stderr_write.reset();

  if (!created) {
    const auto launch_error = GetLastError();
    const auto status = status_for_win32_launch_error(launch_error);
    return python_launch_failure_result(
      config.python_interpreter,
      status,
      launch_error,
      "failed to launch Python interpreter");
  }

  unique_handle process_handle(process.hProcess);
  unique_handle thread_handle(process.hThread);
  if (job_handle.valid()) {
    if (!AssignProcessToJobObject(job_handle.get(), process_handle.get())) {
      TerminateProcess(process_handle.get(), 1);
      return last_error_result(
        execution_termination_reason::launch_failed,
        "failed to assign process to job object");
    }
    if (ResumeThread(thread_handle.get()) == static_cast<DWORD>(-1)) {
      TerminateJobObject(job_handle.get(), 1);
      return last_error_result(
        execution_termination_reason::launch_failed,
        "failed to resume process after job assignment");
    }
  }

  execution_result result;
  add_job_metadata(result, config, request);
  std::thread stdout_thread(
    read_pipe_to_string,
    stdout_read.release(),
    std::ref(result.stdout_text),
    request.limits.max_stdout_bytes,
    std::ref(result.stdout_truncated));
  std::thread stderr_thread(
    read_pipe_to_string,
    stderr_read.release(),
    std::ref(result.stderr_text),
    request.limits.max_stderr_bytes,
    std::ref(result.stderr_truncated));
  std::thread stdin_thread(
    write_string_to_pipe,
    stdin_write.release(),
    std::cref(request.stdin_text));

  const auto started = std::chrono::steady_clock::now();
  bool terminated = false;
  bool timed_out = false;
  bool cancelled = false;

  for (;;) {
    const auto wait_result = WaitForSingleObject(process_handle.get(), 50);
    if (wait_result == WAIT_OBJECT_0) {
      break;
    }

    if (stop_token.stop_requested()) {
      cancelled = true;
      if (job_handle.valid()) {
        TerminateJobObject(job_handle.get(), 1);
      }
      else {
        TerminateProcess(process_handle.get(), 1);
      }
      terminated = true;
      break;
    }

    if (request.limits.timeout.count() > 0 &&
        std::chrono::steady_clock::now() - started >= request.limits.timeout) {
      timed_out = true;
      if (job_handle.valid()) {
        TerminateJobObject(job_handle.get(), 1);
      }
      else {
        TerminateProcess(process_handle.get(), 1);
      }
      terminated = true;
      break;
    }
  }

  if (terminated) {
    WaitForSingleObject(process_handle.get(), INFINITE);
  }

  if (stdin_thread.joinable()) {
    stdin_thread.join();
  }
  if (stdout_thread.joinable()) {
    stdout_thread.join();
  }
  if (stderr_thread.joinable()) {
    stderr_thread.join();
  }

  result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now() - started);
  result.timed_out = timed_out;
  result.cancelled = cancelled;
  if (timed_out) {
    result.termination_reason = execution_termination_reason::timeout;
    result.error_message = "execution timed out";
    return result;
  }
  if (cancelled) {
    result.termination_reason = execution_termination_reason::cancelled;
    result.error_message = "execution cancelled";
    return result;
  }

  DWORD exit_code = 1;
  if (GetExitCodeProcess(process_handle.get(), &exit_code)) {
    result.exit_code = static_cast<int>(exit_code);
  }
  result.termination_reason = execution_termination_reason::exited;
  return result;
}

#endif // _WIN32

} // namespace

std::string to_string(python_interpreter_status status) {
  switch (status) {
    case python_interpreter_status::ok:
      return "ok";
    case python_interpreter_status::empty_path:
      return "empty_path";
    case python_interpreter_status::not_found:
      return "not_found";
    case python_interpreter_status::not_executable:
      return "not_executable";
    case python_interpreter_status::permission_denied:
      return "permission_denied";
    case python_interpreter_status::launch_failed:
      return "launch_failed";
    case python_interpreter_status::startup_timeout:
      return "startup_timeout";
    case python_interpreter_status::invalid_python:
      return "invalid_python";
    case python_interpreter_status::unsupported_version:
      return "unsupported_version";
  }
  return "launch_failed";
}

python_interpreter_probe_result probe_python_interpreter(
  const python_interpreter_probe_request& request) {
  python_interpreter_probe_result result;
  result.resolved_path = request.interpreter;
  result.metadata["python_interpreter"] = request.interpreter.string();
  result.metadata["timeout_ms"] = std::to_string(request.timeout.count());

  if (request.interpreter.empty()) {
    result.status = python_interpreter_status::empty_path;
    result.metadata["error_code"] = stable_error_code_for_status(result.status);
    return result;
  }

#ifdef _WIN32
  const auto explicit_path =
    request.interpreter.is_absolute() || request.interpreter.has_parent_path() ||
    request.interpreter.has_root_name();
  if (explicit_path) {
    std::error_code filesystem_error;
    const auto exists = std::filesystem::exists(request.interpreter, filesystem_error);
    if (filesystem_error) {
      result.status = python_interpreter_status::permission_denied;
      result.system_error = filesystem_error;
      result.metadata["error_code"] = stable_error_code_for_status(result.status);
      result.metadata["filesystem_error_code"] =
        std::to_string(filesystem_error.value());
      result.metadata["filesystem_error_message"] = filesystem_error.message();
      return result;
    }
    if (!exists) {
      result.status = python_interpreter_status::not_found;
      result.metadata["error_code"] = stable_error_code_for_status(result.status);
      return result;
    }
    if (std::filesystem::is_directory(request.interpreter, filesystem_error)) {
      result.status = python_interpreter_status::not_executable;
      result.metadata["error_code"] = stable_error_code_for_status(result.status);
      return result;
    }
  }

  const auto workdir = request.workdir.empty()
                         ? std::filesystem::temp_directory_path() / "wuwe-execution"
                         : request.workdir;
  const auto script_path = probe_script_path_for(workdir);
  try {
    std::filesystem::create_directories(workdir);
    std::ofstream script(script_path, std::ios::binary);
    if (!script) {
      result.status = python_interpreter_status::launch_failed;
      result.metadata["error_code"] = stable_error_code_for_status(result.status);
      result.metadata["probe_error"] = "failed_to_create_probe_script";
      return result;
    }
    script
      << "import sys, json\n"
      << "print(json.dumps({\n"
      << "  \"version\": sys.version,\n"
      << "  \"version_info\": list(sys.version_info[:3]),\n"
      << "  \"executable\": sys.executable,\n"
      << "}))\n";
  }
  catch (const std::exception& error) {
    result.status = python_interpreter_status::launch_failed;
    result.metadata["error_code"] = stable_error_code_for_status(result.status);
    result.metadata["probe_error"] = error.what();
    return result;
  }

  struct probe_script_cleanup {
    std::filesystem::path path;
    ~probe_script_cleanup() {
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
    }
  } cleanup { script_path };

  const auto capture = run_python_script_file(
    request.interpreter,
    script_path,
    workdir,
    request.env,
    request.timeout,
    65536,
    65536);
  result.stdout_text = capture.stdout_text;
  result.stderr_text = capture.stderr_text;
  result.metadata["elapsed_ms"] = std::to_string(capture.elapsed.count());
  if (capture.exit_code.has_value()) {
    result.metadata["exit_code"] = std::to_string(*capture.exit_code);
  }

  if (!capture.created) {
    result.status = status_for_win32_launch_error(capture.launch_error);
    result.system_error = win32_error_code(capture.launch_error);
    result.metadata["error_code"] = stable_error_code_for_status(result.status);
    result.metadata["launch_error_code"] = std::to_string(capture.launch_error);
    result.metadata["launch_error_message"] =
      win32_error_message(capture.launch_error);
    return result;
  }

  if (capture.timed_out) {
    result.status = python_interpreter_status::startup_timeout;
    result.metadata["error_code"] = stable_error_code_for_status(result.status);
    result.metadata["timeout_phase"] = "startup";
    return result;
  }

  if (!capture.exit_code.has_value() || *capture.exit_code != 0) {
    result.status = python_interpreter_status::invalid_python;
    result.metadata["error_code"] = stable_error_code_for_status(result.status);
    return result;
  }

  try {
    const auto parsed = nlohmann::json::parse(capture.stdout_text);
    result.version = parsed.value("version", "");
    result.executable = parsed.value("executable", "");
    if (!result.executable.empty()) {
      result.resolved_path = result.executable;
    }
    result.metadata["version"] = result.version;
    result.metadata["executable"] = result.executable;

    const auto version_info = parsed.at("version_info");
    if (!version_info.is_array() || version_info.empty() ||
        version_info.at(0).get<int>() < 3) {
      result.status = python_interpreter_status::unsupported_version;
      result.metadata["error_code"] = stable_error_code_for_status(result.status);
      return result;
    }
  }
  catch (const std::exception& error) {
    result.status = python_interpreter_status::invalid_python;
    result.metadata["error_code"] = stable_error_code_for_status(result.status);
    result.metadata["parse_error"] = error.what();
    return result;
  }

  result.status = python_interpreter_status::ok;
  result.metadata["error_code"] = stable_error_code_for_status(result.status);
  return result;
#else
  result.status = python_interpreter_status::launch_failed;
  result.metadata["error_code"] = stable_error_code_for_status(result.status);
  result.metadata["probe_error"] =
    "Python interpreter probing is currently implemented only on Windows";
  return result;
#endif
}

controlled_process_backend::controlled_process_backend(
  controlled_process_backend_config config)
    : config_(std::move(config)) {
}

std::optional<execution_result>
controlled_process_backend::validate_python_interpreter(
  const execution_request& request) const {
  if (!config_.validate_python_on_start) {
    return std::nullopt;
  }

#ifdef _WIN32
  const auto probe = probe_python_interpreter({
    .interpreter = config_.python_interpreter,
    .workdir = choose_workdir(config_, request),
    .env = request.env,
    .timeout = config_.python_startup_timeout,
  });
  if (probe.status == python_interpreter_status::ok) {
    return std::nullopt;
  }

  execution_result result {
    .termination_reason = probe.status == python_interpreter_status::startup_timeout
                            ? execution_termination_reason::timeout
                            : execution_termination_reason::launch_failed,
    .timed_out = probe.status == python_interpreter_status::startup_timeout,
    .stdout_text = probe.stdout_text,
    .stderr_text = probe.stderr_text,
    .error_message =
      "Python interpreter validation failed: " + to_string(probe.status),
    .metadata = probe.metadata,
  };
  result.metadata["python_interpreter_probe_status"] = to_string(probe.status);
  result.metadata["python_interpreter"] = config_.python_interpreter.string();
  if (probe.system_error) {
    result.metadata["launch_error_code"] =
      std::to_string(probe.system_error.value());
    result.metadata["launch_error_message"] = probe.system_error.message();
  }
  if (probe.status == python_interpreter_status::startup_timeout) {
    result.metadata["timeout_phase"] = "startup";
  }
  return result;
#else
  (void)request;
  execution_result result {
    .termination_reason = execution_termination_reason::backend_error,
    .error_message =
      "Python interpreter validation is currently implemented only on Windows",
  };
  result.metadata["error_code"] = "python_interpreter_validation_unavailable";
  result.metadata["python_interpreter"] = config_.python_interpreter.string();
  return result;
#endif
}

sandbox::sandbox_backend_info controlled_process_backend::info() const {
  const auto job_enforcement =
#ifdef _WIN32
    config_.use_job_object ? sandbox::enforcement_level::enforced
                           : sandbox::enforcement_level::not_enforced;
#else
    sandbox::enforcement_level::not_enforced;
#endif

  return {
    .name = "controlled_process",
    .isolation = sandbox::isolation_level::controlled_process,
    .features = {
      sandbox::sandbox_feature::environment_allowlist,
      sandbox::sandbox_feature::working_directory,
      sandbox::sandbox_feature::stdout_capture,
      sandbox::sandbox_feature::stderr_capture,
      sandbox::sandbox_feature::timeout,
      sandbox::sandbox_feature::cancellation,
    },
    .enforcement = {
      .shell_execution = sandbox::enforcement_level::enforced,
      .timeout = sandbox::enforcement_level::enforced,
      .cancellation = sandbox::enforcement_level::enforced,
      .stdout_limit = sandbox::enforcement_level::enforced,
      .stderr_limit = sandbox::enforcement_level::enforced,
      .environment_allowlist = sandbox::enforcement_level::enforced,
      .working_directory = sandbox::enforcement_level::enforced,
      .process_tree_cleanup = job_enforcement,
      .process_count_limit = job_enforcement,
      .cpu_time_limit = job_enforcement,
      .memory_limit = job_enforcement,
      .filesystem_read_deny = sandbox::enforcement_level::not_enforced,
      .filesystem_write_deny = sandbox::enforcement_level::not_enforced,
      .network_deny = sandbox::enforcement_level::not_enforced,
    },
  };
}

execution_result controlled_process_backend::run(
  const execution_request& request,
  std::stop_token stop_token) {
  if (request.language != execution_language::python) {
    return {
      .termination_reason = execution_termination_reason::policy_denied,
      .error_message = "controlled process backend only supports Python",
    };
  }

  if (const auto validation_error = validate_python_interpreter(request);
      validation_error.has_value()) {
    return *validation_error;
  }

#ifdef _WIN32
  return launch_python_process(config_, request, stop_token);
#else
  (void)config_;
  (void)request;
  (void)stop_token;
  return {
    .termination_reason = execution_termination_reason::backend_error,
    .error_message =
      "controlled process backend is currently implemented only on Windows",
  };
#endif
}

std::unique_ptr<execution_backend> make_controlled_process_backend(
  controlled_process_backend_config config) {
  return std::make_unique<controlled_process_backend>(std::move(config));
}

} // namespace wuwe::agent::execution
