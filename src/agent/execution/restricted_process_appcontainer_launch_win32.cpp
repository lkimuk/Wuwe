#include "restricted_process_appcontainer_launch_win32.hpp"

#ifdef _WIN32

#include <algorithm>
#include <array>
#include <future>
#include <utility>

namespace wuwe::agent::execution::detail {
namespace {

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

class process_thread_attribute_list {
public:
  process_thread_attribute_list() = default;

  ~process_thread_attribute_list() {
    if (list_ != nullptr) {
      DeleteProcThreadAttributeList(list_);
    }
  }

  process_thread_attribute_list(const process_thread_attribute_list&) = delete;
  process_thread_attribute_list& operator=(const process_thread_attribute_list&) =
    delete;

  [[nodiscard]] bool initialize(DWORD attribute_count) {
    SIZE_T attribute_list_size = 0;
    InitializeProcThreadAttributeList(
      nullptr,
      attribute_count,
      0,
      &attribute_list_size);
    if (attribute_list_size == 0) {
      return false;
    }

    storage_.resize(attribute_list_size);
    list_ = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data());
    if (!InitializeProcThreadAttributeList(
          list_,
          attribute_count,
          0,
          &attribute_list_size)) {
      list_ = nullptr;
      storage_.clear();
      return false;
    }
    return true;
  }

  [[nodiscard]] bool update_handle_list(HANDLE* handles, DWORD handle_count) {
    if (!UpdateProcThreadAttribute(
          list_,
          0,
          PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
          handles,
          sizeof(HANDLE) * handle_count,
          nullptr,
          nullptr)) {
      return false;
    }
    return true;
  }

  [[nodiscard]] bool update_security_capabilities(
    SECURITY_CAPABILITIES& capabilities) {
    if (!UpdateProcThreadAttribute(
          list_,
          0,
          PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES,
          &capabilities,
          sizeof(capabilities),
          nullptr,
          nullptr)) {
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

restricted_appcontainer_launch_result make_launch_result(
  restricted_appcontainer_launch_status status,
  DWORD win32_error = ERROR_SUCCESS,
  std::string detail = {}) {
  return {
    .status = status,
    .win32_error = win32_error,
    .detail = std::move(detail),
  };
}

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

  std::wstring result = L"\"";
  std::size_t backslashes = 0;
  for (const auto ch : arg) {
    if (ch == L'\\') {
      ++backslashes;
      continue;
    }
    if (ch == L'"') {
      result.append(backslashes * 2 + 1, L'\\');
      result.push_back(ch);
      backslashes = 0;
      continue;
    }
    result.append(backslashes, L'\\');
    backslashes = 0;
    result.push_back(ch);
  }
  result.append(backslashes * 2, L'\\');
  result.push_back(L'"');
  return result;
}

bool configure_job(
  HANDLE job,
  const restricted_appcontainer_launch_request& request,
  restricted_appcontainer_launch_result& result) {
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (request.max_process_count > 0) {
    limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
    limits.BasicLimitInformation.ActiveProcessLimit =
      static_cast<DWORD>(request.max_process_count);
  }
  if (request.max_memory_bytes > 0) {
    limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_JOB_MEMORY;
    limits.JobMemoryLimit = static_cast<SIZE_T>(request.max_memory_bytes);
  }
  if (request.max_cpu_time.count() > 0) {
    limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_JOB_TIME;
    limits.BasicLimitInformation.PerJobUserTimeLimit.QuadPart =
      request.max_cpu_time.count() * 10000LL;
  }
  if (!SetInformationJobObject(
        job,
        JobObjectExtendedLimitInformation,
        &limits,
        sizeof(limits))) {
    result = make_launch_result(
      restricted_appcontainer_launch_status::configure_job_failed,
      GetLastError(),
      "SetInformationJobObject");
    return false;
  }
  return true;
}

void terminate_restricted_process(
  HANDLE job,
  HANDLE process,
  UINT exit_code) noexcept {
  if (job != nullptr && job != INVALID_HANDLE_VALUE) {
    TerminateJobObject(job, exit_code);
    return;
  }
  TerminateProcess(process, exit_code);
}

std::string read_pipe_to_limit(
  HANDLE pipe,
  std::size_t limit,
  bool& truncated) {
  unique_handle handle(pipe);
  std::string output;
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
      (std::min<std::size_t>)(remaining, static_cast<std::size_t>(bytes_read));
    if (to_append > 0) {
      output.append(buffer.data(), to_append);
    }
    if (to_append < static_cast<std::size_t>(bytes_read)) {
      truncated = true;
    }
  }
  return output;
}

void write_string_to_handle(HANDLE pipe, std::string text) {
  unique_handle handle(pipe);
  const char* cursor = text.data();
  std::size_t remaining = text.size();
  while (remaining > 0) {
    DWORD bytes_written = 0;
    const auto chunk =
      static_cast<DWORD>((std::min<std::size_t>)(remaining, 65536));
    const auto ok = WriteFile(handle.get(), cursor, chunk, &bytes_written, nullptr);
    if (!ok || bytes_written == 0) {
      break;
    }
    cursor += bytes_written;
    remaining -= bytes_written;
  }
}

std::vector<wchar_t> make_environment_block(
  const std::map<std::wstring, std::wstring>& env) {
  std::wstring block;
  for (const auto& [name, value] : env) {
    block.append(name);
    block.push_back(L'=');
    block.append(value);
    block.push_back(L'\0');
  }
  block.push_back(L'\0');
  if (env.empty()) {
    block.push_back(L'\0');
  }
  return { block.begin(), block.end() };
}

void copy_current_environment_variable_if_present(
  std::map<std::wstring, std::wstring>& environment,
  const wchar_t* name) {
  if (environment.contains(name)) {
    return;
  }
  std::wstring value(32767, L'\0');
  const auto written = GetEnvironmentVariableW(
    name,
    value.data(),
    static_cast<DWORD>(value.size()));
  if (written > 0 && written < value.size()) {
    value.resize(written);
    environment.emplace(name, std::move(value));
  }
}

bool create_pipe_pair(
  PHANDLE read_pipe,
  PHANDLE write_pipe,
  SECURITY_ATTRIBUTES& security_attributes,
  restricted_appcontainer_launch_result& result,
  std::string detail) {
  if (!CreatePipe(read_pipe, write_pipe, &security_attributes, 0)) {
    result = make_launch_result(
      restricted_appcontainer_launch_status::create_pipe_failed,
      GetLastError(),
      std::move(detail));
    return false;
  }
  return true;
}

bool make_handle_non_inheritable(
  HANDLE handle,
  restricted_appcontainer_launch_result& result,
  std::string detail) {
  if (!SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0)) {
    result = make_launch_result(
      restricted_appcontainer_launch_status::set_handle_information_failed,
      GetLastError(),
      std::move(detail));
    return false;
  }
  return true;
}

} // namespace

const char* to_string(restricted_appcontainer_launch_status status) noexcept {
  switch (status) {
    case restricted_appcontainer_launch_status::ok:
      return "ok";
    case restricted_appcontainer_launch_status::invalid_appcontainer_sid:
      return "invalid_appcontainer_sid";
    case restricted_appcontainer_launch_status::create_pipe_failed:
      return "create_pipe_failed";
    case restricted_appcontainer_launch_status::set_handle_information_failed:
      return "set_handle_information_failed";
    case restricted_appcontainer_launch_status::attribute_list_failed:
      return "attribute_list_failed";
    case restricted_appcontainer_launch_status::create_job_failed:
      return "create_job_failed";
    case restricted_appcontainer_launch_status::configure_job_failed:
      return "configure_job_failed";
    case restricted_appcontainer_launch_status::launch_failed:
      return "launch_failed";
    case restricted_appcontainer_launch_status::assign_job_failed:
      return "assign_job_failed";
    case restricted_appcontainer_launch_status::resume_failed:
      return "resume_failed";
    case restricted_appcontainer_launch_status::wait_failed:
      return "wait_failed";
    case restricted_appcontainer_launch_status::get_exit_code_failed:
      return "get_exit_code_failed";
  }
  return "unknown";
}

restricted_appcontainer_launch_result launch_restricted_appcontainer_process(
  restricted_appcontainer_launch_request request) {
  if (request.appcontainer_sid == nullptr) {
    return make_launch_result(
      restricted_appcontainer_launch_status::invalid_appcontainer_sid);
  }

  SECURITY_ATTRIBUTES security_attributes {
    .nLength = sizeof(SECURITY_ATTRIBUTES),
    .lpSecurityDescriptor = nullptr,
    .bInheritHandle = TRUE,
  };

  HANDLE raw_stdin_read = nullptr;
  HANDLE raw_stdin_write = nullptr;
  HANDLE raw_stdout_read = nullptr;
  HANDLE raw_stdout_write = nullptr;
  HANDLE raw_stderr_read = nullptr;
  HANDLE raw_stderr_write = nullptr;
  restricted_appcontainer_launch_result result;
  unique_handle stdin_read;
  unique_handle stdin_write;
  unique_handle stdout_read;
  unique_handle stdout_write;
  unique_handle stderr_read;
  unique_handle stderr_write;

  if (!create_pipe_pair(
        &raw_stdin_read,
        &raw_stdin_write,
        security_attributes,
        result,
        "stdin")) {
    return result;
  }
  stdin_read.reset(raw_stdin_read);
  stdin_write.reset(raw_stdin_write);

  if (!create_pipe_pair(
        &raw_stdout_read,
        &raw_stdout_write,
        security_attributes,
        result,
        "stdout")) {
    return result;
  }
  stdout_read.reset(raw_stdout_read);
  stdout_write.reset(raw_stdout_write);

  if (!create_pipe_pair(
        &raw_stderr_read,
        &raw_stderr_write,
        security_attributes,
        result,
        "stderr")) {
    return result;
  }
  stderr_read.reset(raw_stderr_read);
  stderr_write.reset(raw_stderr_write);

  if (!make_handle_non_inheritable(stdin_write.get(), result, "stdin_write") ||
      !make_handle_non_inheritable(stdout_read.get(), result, "stdout_read") ||
      !make_handle_non_inheritable(stderr_read.get(), result, "stderr_read")) {
    return result;
  }

  STARTUPINFOEXW startup_ex {};
  startup_ex.StartupInfo.cb = sizeof(startup_ex);
  startup_ex.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup_ex.StartupInfo.hStdInput = stdin_read.get();
  startup_ex.StartupInfo.hStdOutput = stdout_write.get();
  startup_ex.StartupInfo.hStdError = stderr_write.get();

  HANDLE inherited_handles[] {
    stdin_read.get(),
    stdout_write.get(),
    stderr_write.get(),
  };
  SECURITY_CAPABILITIES capabilities {};
  capabilities.AppContainerSid = request.appcontainer_sid;
  capabilities.Capabilities = nullptr;
  capabilities.CapabilityCount = 0;
  process_thread_attribute_list attribute_list;
  if (!attribute_list.initialize(2) ||
      !attribute_list.update_handle_list(
        inherited_handles,
        static_cast<DWORD>(std::size(inherited_handles))) ||
      !attribute_list.update_security_capabilities(capabilities)) {
    return make_launch_result(
      restricted_appcontainer_launch_status::attribute_list_failed,
      GetLastError(),
      "PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES");
  }
  startup_ex.lpAttributeList = attribute_list.get();

  unique_handle job;
  if (request.use_job_object) {
    job.reset(CreateJobObjectW(nullptr, nullptr));
    if (!job.valid()) {
      return make_launch_result(
        restricted_appcontainer_launch_status::create_job_failed,
        GetLastError(),
        "CreateJobObjectW");
    }
    if (!configure_job(job.get(), request, result)) {
      return result;
    }
  }

  auto command_line = quote_windows_arg(request.executable.wstring());
  for (const auto& argument : request.arguments) {
    command_line += L" ";
    command_line += quote_windows_arg(argument);
  }

  const auto working_directory_text = request.working_directory.empty()
                                      ? std::wstring {}
                                      : request.working_directory.wstring();
  std::vector<wchar_t> environment_block;
  if (request.environment.has_value()) {
    for (const auto* name : {
           L"SystemRoot",
           L"USERPROFILE",
           L"LOCALAPPDATA",
           L"APPDATA",
           L"TEMP",
           L"TMP",
         }) {
      copy_current_environment_variable_if_present(*request.environment, name);
    }
    environment_block = make_environment_block(*request.environment);
  }

  DWORD creation_flags = CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT |
                         CREATE_SUSPENDED;
  if (request.environment.has_value()) {
    creation_flags |= CREATE_UNICODE_ENVIRONMENT;
  }

  PROCESS_INFORMATION process {};
  const auto created = CreateProcessW(
    nullptr,
    command_line.data(),
    nullptr,
    nullptr,
    TRUE,
    creation_flags,
    request.environment.has_value()
      ? static_cast<LPVOID>(environment_block.data())
      : nullptr,
    working_directory_text.empty() ? nullptr : working_directory_text.c_str(),
    &startup_ex.StartupInfo,
    &process);
  if (!created) {
    return make_launch_result(
      restricted_appcontainer_launch_status::launch_failed,
      GetLastError(),
      request.executable.string());
  }

  unique_handle process_handle(process.hProcess);
  unique_handle thread_handle(process.hThread);
  stdin_read.reset();
  stdout_write.reset();
  stderr_write.reset();

  if (request.use_job_object) {
    if (!AssignProcessToJobObject(job.get(), process_handle.get())) {
      terminate_restricted_process(job.get(), process_handle.get(), 1);
      return make_launch_result(
        restricted_appcontainer_launch_status::assign_job_failed,
        GetLastError(),
        "AssignProcessToJobObject");
    }
  }
  if (ResumeThread(thread_handle.get()) == static_cast<DWORD>(-1)) {
    terminate_restricted_process(job.get(), process_handle.get(), 1);
    return make_launch_result(
      restricted_appcontainer_launch_status::resume_failed,
      GetLastError(),
      "ResumeThread");
  }

  auto stdin_future = std::async(
    std::launch::async,
    write_string_to_handle,
    stdin_write.release(),
    std::move(request.stdin_text));
  bool stdout_truncated = false;
  bool stderr_truncated = false;
  auto stdout_future = std::async(
    std::launch::async,
    read_pipe_to_limit,
    stdout_read.release(),
    request.max_stdout_bytes,
    std::ref(stdout_truncated));
  auto stderr_future = std::async(
    std::launch::async,
    read_pipe_to_limit,
    stderr_read.release(),
    request.max_stderr_bytes,
    std::ref(stderr_truncated));

  result = {};
  const auto started = std::chrono::steady_clock::now();
  for (;;) {
    const auto wait_result = WaitForSingleObject(process_handle.get(), 50);
    if (wait_result == WAIT_OBJECT_0) {
      break;
    }
    if (wait_result != WAIT_TIMEOUT) {
      terminate_restricted_process(job.get(), process_handle.get(), 1);
      WaitForSingleObject(process_handle.get(), INFINITE);
      result.status = restricted_appcontainer_launch_status::wait_failed;
      result.win32_error = GetLastError();
      result.detail = "WaitForSingleObject";
      break;
    }

    if (request.stop_token.stop_requested()) {
      result.capture.cancelled = true;
      terminate_restricted_process(job.get(), process_handle.get(), 1);
      WaitForSingleObject(process_handle.get(), INFINITE);
      break;
    }
    if (request.timeout.count() > 0 &&
        std::chrono::steady_clock::now() - started >= request.timeout) {
      result.capture.timed_out = true;
      terminate_restricted_process(job.get(), process_handle.get(), 1);
      WaitForSingleObject(process_handle.get(), INFINITE);
      break;
    }
  }

  if (stdin_future.valid()) {
    stdin_future.get();
  }

  if (!GetExitCodeProcess(process_handle.get(), &result.capture.exit_code)) {
    result.status = restricted_appcontainer_launch_status::get_exit_code_failed;
    result.win32_error = GetLastError();
    result.detail = "GetExitCodeProcess";
  }
  result.capture.stdout_text = stdout_future.get();
  result.capture.stderr_text = stderr_future.get();
  result.capture.stdout_truncated = stdout_truncated;
  result.capture.stderr_truncated = stderr_truncated;
  return result;
}

} // namespace wuwe::agent::execution::detail

#endif // _WIN32
