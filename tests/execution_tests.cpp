#include <cassert>
#include <array>
#include <chrono>
#include <stdexcept>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <string_view>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include <wuwe/agent/approval/approval_service.hpp>
#include <wuwe/agent/audit/audit_sink.hpp>
#include <wuwe/agent/execution/execution.hpp>
#include <wuwe/agent/sandbox/sandbox.hpp>

namespace execution = wuwe::agent::execution;
namespace sandbox = wuwe::agent::sandbox;

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <aclapi.h>
#include <sddl.h>
#include <shellapi.h>
#include <userenv.h>
#include <windows.h>

class test_unique_handle {
public:
  test_unique_handle() = default;
  explicit test_unique_handle(HANDLE handle) noexcept : handle_(handle) {
  }

  ~test_unique_handle() {
    reset();
  }

  test_unique_handle(const test_unique_handle&) = delete;
  test_unique_handle& operator=(const test_unique_handle&) = delete;

  test_unique_handle(test_unique_handle&& other) noexcept : handle_(other.release()) {
  }

  test_unique_handle& operator=(test_unique_handle&& other) noexcept {
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

class test_process_thread_attribute_list {
public:
  test_process_thread_attribute_list() = default;

  ~test_process_thread_attribute_list() {
    if (list_ != nullptr) {
      DeleteProcThreadAttributeList(list_);
    }
  }

  test_process_thread_attribute_list(const test_process_thread_attribute_list&) =
    delete;
  test_process_thread_attribute_list& operator=(
    const test_process_thread_attribute_list&) = delete;

  [[nodiscard]] bool initialize_with_handle_list(
    HANDLE* handles,
    DWORD handle_count) {
    return initialize(1) && update_handle_list(handles, handle_count);
  }

  [[nodiscard]] bool initialize(DWORD attribute_count) {
    SIZE_T attribute_list_size = 0;
    InitializeProcThreadAttributeList(nullptr, attribute_count, 0, &attribute_list_size);
    if (attribute_list_size == 0) {
      return false;
    }

    storage_.resize(attribute_list_size);
    list_ = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data());
    if (!InitializeProcThreadAttributeList(list_, attribute_count, 0, &attribute_list_size)) {
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
      DeleteProcThreadAttributeList(list_);
      list_ = nullptr;
      storage_.clear();
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

void require_condition(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

void require_win32(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(
      std::string(message) + " failed with GetLastError=" +
      std::to_string(GetLastError()));
  }
}

void require_error_code(DWORD actual, DWORD expected, std::string_view message) {
  if (actual != expected) {
    throw std::runtime_error(
      std::string(message) + " expected=" + std::to_string(expected) +
      " actual=" + std::to_string(actual));
  }
}

std::wstring current_test_executable_path() {
  std::wstring path(MAX_PATH, L'\0');
  for (;;) {
    const auto written = GetModuleFileNameW(
      nullptr,
      path.data(),
      static_cast<DWORD>(path.size()));
    require_win32(written != 0, "GetModuleFileNameW");
    if (written < path.size()) {
      path.resize(written);
      return path;
    }
    path.resize(path.size() * 2);
  }
}

std::string read_pipe_to_end(HANDLE pipe) {
  test_unique_handle handle(pipe);
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
    output.append(buffer.data(), bytes_read);
  }
  return output;
}

struct child_process_capture {
  DWORD exit_code { 1 };
  std::string stdout_text;
  std::string stderr_text;
};

void configure_probe_job(HANDLE job) {
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
  limits.BasicLimitInformation.LimitFlags =
    JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
  limits.BasicLimitInformation.ActiveProcessLimit = 1;
  require_win32(
    SetInformationJobObject(
      job,
      JobObjectExtendedLimitInformation,
      &limits,
      sizeof(limits)) != FALSE,
    "SetInformationJobObject");
}

bool token_has_restrictions(HANDLE token) {
  DWORD has_restrictions = 0;
  DWORD returned = 0;
  require_win32(
    GetTokenInformation(
      token,
      TokenHasRestrictions,
      &has_restrictions,
      sizeof(has_restrictions),
      &returned) != FALSE,
    "GetTokenInformation(TokenHasRestrictions)");
  return has_restrictions != 0;
}

std::vector<BYTE> current_user_sid() {
  HANDLE raw_token = nullptr;
  require_win32(
    OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token) != FALSE,
    "OpenProcessToken(current user)");
  test_unique_handle token(raw_token);

  DWORD token_user_size = 0;
  GetTokenInformation(token.get(), TokenUser, nullptr, 0, &token_user_size);
  require_error_code(
    GetLastError(),
    ERROR_INSUFFICIENT_BUFFER,
    "GetTokenInformation(TokenUser size)");

  std::vector<BYTE> token_user_storage(token_user_size);
  require_win32(
    GetTokenInformation(
      token.get(),
      TokenUser,
      token_user_storage.data(),
      static_cast<DWORD>(token_user_storage.size()),
      &token_user_size) != FALSE,
    "GetTokenInformation(TokenUser)");
  const auto* token_user =
    reinterpret_cast<const TOKEN_USER*>(token_user_storage.data());

  DWORD sid_size = GetLengthSid(token_user->User.Sid);
  std::vector<BYTE> sid_storage(sid_size);
  require_win32(
    CopySid(sid_size, sid_storage.data(), token_user->User.Sid) != FALSE,
    "CopySid(current user)");
  return sid_storage;
}

std::vector<BYTE> current_logon_sid() {
  HANDLE raw_token = nullptr;
  require_win32(
    OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token) != FALSE,
    "OpenProcessToken(logon SID)");
  test_unique_handle token(raw_token);

  DWORD token_groups_size = 0;
  GetTokenInformation(token.get(), TokenGroups, nullptr, 0, &token_groups_size);
  require_error_code(
    GetLastError(),
    ERROR_INSUFFICIENT_BUFFER,
    "GetTokenInformation(TokenGroups size)");

  std::vector<BYTE> token_groups_storage(token_groups_size);
  require_win32(
    GetTokenInformation(
      token.get(),
      TokenGroups,
      token_groups_storage.data(),
      static_cast<DWORD>(token_groups_storage.size()),
      &token_groups_size) != FALSE,
    "GetTokenInformation(TokenGroups)");
  const auto* token_groups =
    reinterpret_cast<const TOKEN_GROUPS*>(token_groups_storage.data());

  for (DWORD index = 0; index < token_groups->GroupCount; ++index) {
    const auto& group = token_groups->Groups[index];
    if ((group.Attributes & SE_GROUP_LOGON_ID) == 0) {
      continue;
    }
    DWORD sid_size = GetLengthSid(group.Sid);
    std::vector<BYTE> sid_storage(sid_size);
    require_win32(
      CopySid(sid_size, sid_storage.data(), group.Sid) != FALSE,
      "CopySid(logon SID)");
    return sid_storage;
  }

  throw std::runtime_error("current token does not contain a logon SID");
}

std::vector<BYTE> well_known_sid(WELL_KNOWN_SID_TYPE type) {
  std::vector<BYTE> sid_storage(SECURITY_MAX_SID_SIZE);
  DWORD sid_size = static_cast<DWORD>(sid_storage.size());
  require_win32(
    CreateWellKnownSid(type, nullptr, sid_storage.data(), &sid_size) != FALSE,
    "CreateWellKnownSid");
  sid_storage.resize(sid_size);
  return sid_storage;
}

EXPLICIT_ACCESSW explicit_access_for_sid(
  const void* sid,
  DWORD access_permissions,
  DWORD inheritance = NO_INHERITANCE) {
  EXPLICIT_ACCESSW entry {};
  entry.grfAccessPermissions = access_permissions;
  entry.grfAccessMode = SET_ACCESS;
  entry.grfInheritance = inheritance;
  entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
  entry.Trustee.TrusteeType = TRUSTEE_IS_UNKNOWN;
  entry.Trustee.ptstrName =
    reinterpret_cast<LPWSTR>(const_cast<void*>(sid));
  return entry;
}

void set_protected_dacl(
  const std::filesystem::path& path,
  std::vector<EXPLICIT_ACCESSW> entries) {
  PACL raw_acl = nullptr;
  const auto acl_error = SetEntriesInAclW(
    static_cast<ULONG>(entries.size()),
    entries.data(),
    nullptr,
    &raw_acl);
  require_error_code(acl_error, ERROR_SUCCESS, "SetEntriesInAclW");
  struct acl_cleanup {
    PACL acl;
    ~acl_cleanup() {
      if (acl != nullptr) {
        LocalFree(acl);
      }
    }
  } cleanup { raw_acl };

  auto path_text = path.wstring();
  const auto security_error = SetNamedSecurityInfoW(
    path_text.data(),
    SE_FILE_OBJECT,
    DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
    nullptr,
    nullptr,
    raw_acl,
    nullptr);
  require_error_code(security_error, ERROR_SUCCESS, "SetNamedSecurityInfoW");
}

void set_probe_file_dacl(
  const std::filesystem::path& path,
  const void* current_user,
  const void* builtin_users,
  DWORD builtin_users_access) {
  const auto local_system = well_known_sid(WinLocalSystemSid);
  const auto administrators = well_known_sid(WinBuiltinAdministratorsSid);
  std::vector<EXPLICIT_ACCESSW> entries {
    explicit_access_for_sid(current_user, GENERIC_ALL),
    explicit_access_for_sid(local_system.data(), GENERIC_ALL),
    explicit_access_for_sid(administrators.data(), GENERIC_ALL),
  };
  if (builtin_users_access != 0) {
    entries.push_back(
      explicit_access_for_sid(builtin_users, builtin_users_access));
  }
  set_protected_dacl(path, std::move(entries));
}

void set_probe_directory_dacl(
  const std::filesystem::path& path,
  const void* current_user,
  const void* builtin_users,
  DWORD builtin_users_access) {
  const auto local_system = well_known_sid(WinLocalSystemSid);
  const auto administrators = well_known_sid(WinBuiltinAdministratorsSid);
  constexpr DWORD inherit_to_children =
    CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE;
  std::vector<EXPLICIT_ACCESSW> entries {
    explicit_access_for_sid(current_user, GENERIC_ALL),
    explicit_access_for_sid(current_user, GENERIC_ALL, inherit_to_children),
    explicit_access_for_sid(local_system.data(), GENERIC_ALL),
    explicit_access_for_sid(
      local_system.data(),
      GENERIC_ALL,
      inherit_to_children),
    explicit_access_for_sid(administrators.data(), GENERIC_ALL),
    explicit_access_for_sid(
      administrators.data(),
      GENERIC_ALL,
      inherit_to_children),
  };
  if (builtin_users_access != 0) {
    entries.push_back(explicit_access_for_sid(
      builtin_users,
      builtin_users_access));
    entries.push_back(explicit_access_for_sid(
      builtin_users,
      builtin_users_access,
      inherit_to_children));
  }
  set_protected_dacl(path, std::move(entries));
}

test_unique_handle create_builtin_users_restricted_token() {
  HANDLE raw_current_token = nullptr;
  require_win32(
    OpenProcessToken(
      GetCurrentProcess(),
      TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY | TOKEN_QUERY | TOKEN_ADJUST_DEFAULT |
        TOKEN_ADJUST_SESSIONID,
      &raw_current_token) != FALSE,
    "OpenProcessToken(restricted token source)");
  test_unique_handle current_token(raw_current_token);

  const auto builtin_users = well_known_sid(WinBuiltinUsersSid);
  const auto authenticated_users = well_known_sid(WinAuthenticatedUserSid);
  const auto interactive_users = well_known_sid(WinInteractiveSid);
  const auto logon_sid = current_logon_sid();
  SID_AND_ATTRIBUTES restricting_sids[] {
    {
      .Sid = const_cast<BYTE*>(builtin_users.data()),
      .Attributes = 0,
    },
    {
      .Sid = const_cast<BYTE*>(authenticated_users.data()),
      .Attributes = 0,
    },
    {
      .Sid = const_cast<BYTE*>(interactive_users.data()),
      .Attributes = 0,
    },
    {
      .Sid = const_cast<BYTE*>(logon_sid.data()),
      .Attributes = 0,
    },
  };

  HANDLE raw_restricted_token = nullptr;
  require_win32(
    CreateRestrictedToken(
      current_token.get(),
      DISABLE_MAX_PRIVILEGE,
      0,
      nullptr,
      0,
      nullptr,
      static_cast<DWORD>(std::size(restricting_sids)),
      restricting_sids,
      &raw_restricted_token) != FALSE,
    "CreateRestrictedToken(restricting SIDs)");
  test_unique_handle restricted_token(raw_restricted_token);
  require_condition(
    token_has_restrictions(restricted_token.get()),
    "restricted token should report TokenHasRestrictions");
  return restricted_token;
}

child_process_capture run_appcontainer_probe_child(
  const std::filesystem::path& executable,
  PSID appcontainer_sid,
  const std::vector<std::wstring>& arguments,
  const std::filesystem::path& working_directory = {}) {
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
  require_win32(
    CreatePipe(&raw_stdin_read, &raw_stdin_write, &security_attributes, 0) != FALSE,
    "CreatePipe(appcontainer probe stdin)");
  require_win32(
    CreatePipe(&raw_stdout_read, &raw_stdout_write, &security_attributes, 0) != FALSE,
    "CreatePipe(appcontainer probe stdout)");
  require_win32(
    CreatePipe(&raw_stderr_read, &raw_stderr_write, &security_attributes, 0) != FALSE,
    "CreatePipe(appcontainer probe stderr)");

  test_unique_handle stdin_read(raw_stdin_read);
  test_unique_handle stdin_write(raw_stdin_write);
  test_unique_handle stdout_read(raw_stdout_read);
  test_unique_handle stdout_write(raw_stdout_write);
  test_unique_handle stderr_read(raw_stderr_read);
  test_unique_handle stderr_write(raw_stderr_write);

  require_win32(
    SetHandleInformation(stdin_write.get(), HANDLE_FLAG_INHERIT, 0) != FALSE,
    "SetHandleInformation(appcontainer probe stdin_write)");
  require_win32(
    SetHandleInformation(stdout_read.get(), HANDLE_FLAG_INHERIT, 0) != FALSE,
    "SetHandleInformation(appcontainer probe stdout_read)");
  require_win32(
    SetHandleInformation(stderr_read.get(), HANDLE_FLAG_INHERIT, 0) != FALSE,
    "SetHandleInformation(appcontainer probe stderr_read)");

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
  capabilities.AppContainerSid = appcontainer_sid;
  capabilities.Capabilities = nullptr;
  capabilities.CapabilityCount = 0;
  test_process_thread_attribute_list attribute_list;
  require_condition(
    attribute_list.initialize(2),
    "initialize AppContainer probe attribute list");
  require_condition(
    attribute_list.update_handle_list(
      inherited_handles,
      static_cast<DWORD>(std::size(inherited_handles))),
    "update AppContainer probe inherited handle list");
  require_condition(
    attribute_list.update_security_capabilities(capabilities),
    "update AppContainer probe security capabilities");
  startup_ex.lpAttributeList = attribute_list.get();

  test_unique_handle job(CreateJobObjectW(nullptr, nullptr));
  require_win32(job.valid(), "CreateJobObjectW(appcontainer probe)");
  configure_probe_job(job.get());

  auto command_line = quote_windows_arg(executable.wstring());
  for (const auto& argument : arguments) {
    command_line += L" ";
    command_line += quote_windows_arg(argument);
  }
  const auto working_directory_text = working_directory.empty()
                                      ? std::wstring {}
                                      : working_directory.wstring();
  PROCESS_INFORMATION process {};
  const auto created = CreateProcessW(
    nullptr,
    command_line.data(),
    nullptr,
    nullptr,
    TRUE,
    CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED,
    nullptr,
    working_directory_text.empty() ? nullptr : working_directory_text.c_str(),
    &startup_ex.StartupInfo,
    &process);
  require_win32(created != FALSE, "CreateProcessW(appcontainer probe child)");

  test_unique_handle process_handle(process.hProcess);
  test_unique_handle thread_handle(process.hThread);
  stdin_read.reset();
  stdout_write.reset();
  stderr_write.reset();

  require_win32(
    AssignProcessToJobObject(job.get(), process_handle.get()) != FALSE,
    "AssignProcessToJobObject(appcontainer probe)");
  require_win32(
    ResumeThread(thread_handle.get()) != static_cast<DWORD>(-1),
    "ResumeThread(appcontainer probe)");

  stdin_write.reset();
  auto stdout_future = std::async(
    std::launch::async,
    read_pipe_to_end,
    stdout_read.release());
  auto stderr_future = std::async(
    std::launch::async,
    read_pipe_to_end,
    stderr_read.release());

  const auto wait_result = WaitForSingleObject(process_handle.get(), 5000);
  require_condition(
    wait_result == WAIT_OBJECT_0,
    "AppContainer probe child did not exit within 5000 ms");

  child_process_capture capture;
  require_win32(
    GetExitCodeProcess(process_handle.get(), &capture.exit_code) != FALSE,
    "GetExitCodeProcess(appcontainer probe)");
  capture.stdout_text = stdout_future.get();
  capture.stderr_text = stderr_future.get();
  return capture;
}

std::filesystem::path make_probe_run_directory(std::string_view name) {
  const auto root = std::filesystem::current_path() / "wuwe-restricted-probes";
  const auto path =
    root / (std::string(name) + "-" + std::to_string(GetCurrentProcessId()));
  std::error_code ignored;
  std::filesystem::remove_all(path, ignored);
  std::filesystem::create_directories(path);
  return path;
}

class probe_directory_cleanup {
public:
  explicit probe_directory_cleanup(std::filesystem::path path)
      : path_(std::move(path)) {
  }

  ~probe_directory_cleanup() noexcept {
    cleanup();
  }

  probe_directory_cleanup(const probe_directory_cleanup&) = delete;
  probe_directory_cleanup& operator=(const probe_directory_cleanup&) = delete;

private:
  static void restore_cleanup_dacl(
    const std::filesystem::path& path,
    bool is_directory,
    const void* current_user,
    const void* builtin_users) noexcept {
    try {
      if (is_directory) {
        set_probe_directory_dacl(
          path,
          current_user,
          builtin_users,
          FILE_GENERIC_READ | FILE_GENERIC_EXECUTE);
      }
      else {
        set_probe_file_dacl(
          path,
          current_user,
          builtin_users,
          FILE_GENERIC_READ | FILE_GENERIC_EXECUTE);
      }
    }
    catch (const std::exception& error) {
      std::cerr << "failed to restore restricted probe ACL for "
                << path.string() << ": " << error.what() << "\n";
    }
  }

  void cleanup() noexcept {
    std::error_code ignored;
    if (path_.empty() || !std::filesystem::exists(path_, ignored)) {
      return;
    }

    std::vector<BYTE> current_user;
    std::vector<BYTE> builtin_users;
    try {
      current_user = current_user_sid();
      builtin_users = well_known_sid(WinBuiltinUsersSid);
    }
    catch (const std::exception& error) {
      std::cerr << "failed to collect SIDs for restricted probe cleanup: "
                << error.what() << "\n";
    }

    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(
           path_,
           std::filesystem::directory_options::skip_permission_denied,
           ignored)) {
      ignored.clear();
      const auto is_directory = entry.is_directory(ignored);
      if (!ignored && is_directory && !current_user.empty() && !builtin_users.empty()) {
        restore_cleanup_dacl(
          entry.path(),
          true,
          current_user.data(),
          builtin_users.data());
        continue;
      }

      ignored.clear();
      const auto is_regular_file = entry.is_regular_file(ignored);
      if (!ignored && is_regular_file && !current_user.empty() && !builtin_users.empty()) {
        restore_cleanup_dacl(
          entry.path(),
          false,
          current_user.data(),
          builtin_users.data());
      }
    }
    if (!current_user.empty() && !builtin_users.empty()) {
      restore_cleanup_dacl(
        path_,
        true,
        current_user.data(),
        builtin_users.data());
    }

    ignored.clear();
    std::filesystem::remove_all(path_, ignored);
    if (ignored) {
      std::cerr << "failed to remove restricted probe directory "
                << path_.string() << ": " << ignored.message() << "\n";
    }
  }

  std::filesystem::path path_;
};

class sid_ptr {
public:
  sid_ptr() = default;
  explicit sid_ptr(PSID sid) noexcept : sid_(sid) {
  }

  ~sid_ptr() {
    reset();
  }

  sid_ptr(const sid_ptr&) = delete;
  sid_ptr& operator=(const sid_ptr&) = delete;

  sid_ptr(sid_ptr&& other) noexcept : sid_(other.release()) {
  }

  sid_ptr& operator=(sid_ptr&& other) noexcept {
    if (this != &other) {
      reset(other.release());
    }
    return *this;
  }

  [[nodiscard]] PSID get() const noexcept {
    return sid_;
  }

  [[nodiscard]] PSID release() noexcept {
    const auto sid = sid_;
    sid_ = nullptr;
    return sid;
  }

  void reset(PSID sid = nullptr) noexcept {
    if (sid_ != nullptr) {
      FreeSid(sid_);
    }
    sid_ = sid;
  }

private:
  PSID sid_ {};
};

class command_line_args {
public:
  command_line_args() {
    argv_ = CommandLineToArgvW(GetCommandLineW(), &argc_);
    require_win32(argv_ != nullptr, "CommandLineToArgvW");
  }

  ~command_line_args() {
    if (argv_ != nullptr) {
      LocalFree(argv_);
    }
  }

  command_line_args(const command_line_args&) = delete;
  command_line_args& operator=(const command_line_args&) = delete;

  [[nodiscard]] int argc() const noexcept {
    return argc_;
  }

  [[nodiscard]] std::wstring_view at(int index) const {
    require_condition(index >= 0 && index < argc_, "command line argument index out of range");
    return argv_[index];
  }

private:
  int argc_ { 0 };
  LPWSTR* argv_ { nullptr };
};

class winsock_session {
public:
  winsock_session() {
    WSADATA data {};
    started_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }

  ~winsock_session() {
    if (started_) {
      WSACleanup();
    }
  }

  [[nodiscard]] bool started() const noexcept {
    return started_;
  }

private:
  bool started_ { false };
};

std::string appcontainer_profile_name() {
  return "wuwe-restricted-probe-" + std::to_string(GetCurrentProcessId());
}

std::wstring widen_ascii(std::string_view text) {
  std::wstring result;
  result.reserve(text.size());
  for (const char ch : text) {
    result.push_back(static_cast<unsigned char>(ch));
  }
  return result;
}

std::filesystem::path appcontainer_storage_path(PSID appcontainer_sid) {
  LPWSTR raw_sid_text = nullptr;
  require_win32(
    ConvertSidToStringSidW(appcontainer_sid, &raw_sid_text) != FALSE,
    "ConvertSidToStringSidW(appcontainer)");
  struct sid_text_cleanup {
    LPWSTR text;
    ~sid_text_cleanup() {
      if (text != nullptr) {
        LocalFree(text);
      }
    }
  } cleanup_sid { raw_sid_text };

  PWSTR raw_path = nullptr;
  const auto result = GetAppContainerFolderPath(raw_sid_text, &raw_path);
  require_condition(
    SUCCEEDED(result),
    "GetAppContainerFolderPath should return the test profile storage path");
  struct path_cleanup {
    PWSTR path;
    ~path_cleanup() {
      if (path != nullptr) {
        CoTaskMemFree(path);
      }
    }
  } cleanup { raw_path };
  return std::filesystem::path(raw_path);
}

void grant_file_access_to_sid(
  const std::filesystem::path& path,
  PSID sid,
  DWORD access_permissions) {
  const auto current_user = current_user_sid();
  const auto local_system = well_known_sid(WinLocalSystemSid);
  const auto administrators = well_known_sid(WinBuiltinAdministratorsSid);
  std::vector<EXPLICIT_ACCESSW> entries {
    explicit_access_for_sid(current_user.data(), GENERIC_ALL),
    explicit_access_for_sid(local_system.data(), GENERIC_ALL),
    explicit_access_for_sid(administrators.data(), GENERIC_ALL),
    explicit_access_for_sid(sid, access_permissions),
  };
  set_protected_dacl(path, std::move(entries));
}

void grant_directory_access_to_sid(
  const std::filesystem::path& path,
  PSID sid,
  DWORD access_permissions) {
  const auto current_user = current_user_sid();
  const auto local_system = well_known_sid(WinLocalSystemSid);
  const auto administrators = well_known_sid(WinBuiltinAdministratorsSid);
  constexpr DWORD inherit_to_children =
    CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE;
  std::vector<EXPLICIT_ACCESSW> entries {
    explicit_access_for_sid(current_user.data(), GENERIC_ALL),
    explicit_access_for_sid(current_user.data(), GENERIC_ALL, inherit_to_children),
    explicit_access_for_sid(local_system.data(), GENERIC_ALL),
    explicit_access_for_sid(local_system.data(), GENERIC_ALL, inherit_to_children),
    explicit_access_for_sid(administrators.data(), GENERIC_ALL),
    explicit_access_for_sid(administrators.data(), GENERIC_ALL, inherit_to_children),
    explicit_access_for_sid(sid, access_permissions),
    explicit_access_for_sid(sid, access_permissions, inherit_to_children),
  };
  set_protected_dacl(path, std::move(entries));
}

bool token_can_access_path(
  HANDLE token,
  const std::filesystem::path& path,
  DWORD desired_access) {
  PSECURITY_DESCRIPTOR raw_security_descriptor = nullptr;
  auto path_text = path.wstring();
  const auto security_error = GetNamedSecurityInfoW(
    path_text.data(),
    SE_FILE_OBJECT,
    OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION |
      DACL_SECURITY_INFORMATION,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    &raw_security_descriptor);
  require_error_code(security_error, ERROR_SUCCESS, "GetNamedSecurityInfoW");
  struct security_descriptor_cleanup {
    PSECURITY_DESCRIPTOR descriptor;
    ~security_descriptor_cleanup() {
      if (descriptor != nullptr) {
        LocalFree(descriptor);
      }
    }
  } cleanup { raw_security_descriptor };

  HANDLE raw_impersonation_token = nullptr;
  require_win32(
    DuplicateToken(
      token,
      SecurityImpersonation,
      &raw_impersonation_token) != FALSE,
    "DuplicateToken");
  test_unique_handle impersonation_token(raw_impersonation_token);

  GENERIC_MAPPING file_mapping {
    .GenericRead = FILE_GENERIC_READ,
    .GenericWrite = FILE_GENERIC_WRITE,
    .GenericExecute = FILE_GENERIC_EXECUTE,
    .GenericAll = FILE_ALL_ACCESS,
  };
  MapGenericMask(&desired_access, &file_mapping);

  PRIVILEGE_SET privileges {};
  DWORD privilege_size = sizeof(privileges);
  DWORD granted_access = 0;
  BOOL access_status = FALSE;
  require_win32(
    AccessCheck(
      raw_security_descriptor,
      impersonation_token.get(),
      desired_access,
      &file_mapping,
      &privileges,
      &privilege_size,
      &granted_access,
      &access_status) != FALSE,
    "AccessCheck");
  return access_status != FALSE;
}

int restricted_token_probe_child_main() {
  HANDLE raw_token = nullptr;
  require_win32(
    OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token) != FALSE,
    "OpenProcessToken(child)");
  test_unique_handle token(raw_token);

  const auto has_restrictions = token_has_restrictions(token.get());
  DWORD session_id = 0;
  DWORD returned = 0;
  require_win32(
    GetTokenInformation(
      token.get(),
      TokenSessionId,
      &session_id,
      sizeof(session_id),
      &returned) != FALSE,
    "GetTokenInformation(TokenSessionId)");

  const auto comspec_present = GetEnvironmentVariableA("COMSPEC", nullptr, 0) > 0;
  std::cout << "has_restrictions=" << (has_restrictions ? "1" : "0") << "\n";
  std::cout << "session_id=" << session_id << "\n";
  std::cout << "comspec_present=" << (comspec_present ? "1" : "0") << "\n";
  return has_restrictions ? 0 : 2;
}

bool probe_child_can_read_file(const std::wstring& path) {
  test_unique_handle file(CreateFileW(
    path.c_str(),
    GENERIC_READ,
    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
    nullptr,
    OPEN_EXISTING,
    FILE_ATTRIBUTE_NORMAL,
    nullptr));
  if (!file.valid()) {
    return false;
  }
  char buffer = 0;
  DWORD bytes_read = 0;
  return ReadFile(file.get(), &buffer, sizeof(buffer), &bytes_read, nullptr) != FALSE &&
         bytes_read > 0;
}

bool probe_child_can_write_file(const std::wstring& path) {
  test_unique_handle file(CreateFileW(
    path.c_str(),
    GENERIC_WRITE,
    0,
    nullptr,
    CREATE_ALWAYS,
    FILE_ATTRIBUTE_NORMAL,
    nullptr));
  if (!file.valid()) {
    return false;
  }
  const char payload[] = "restricted-child-write-ok";
  DWORD bytes_written = 0;
  return WriteFile(
           file.get(),
           payload,
           static_cast<DWORD>(sizeof(payload) - 1),
           &bytes_written,
           nullptr) != FALSE &&
         bytes_written == sizeof(payload) - 1;
}

void probe_child_write_stdout(std::string_view text) {
  const auto handle = GetStdHandle(STD_OUTPUT_HANDLE);
  if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
    return;
  }
  DWORD bytes_written = 0;
  WriteFile(
    handle,
    text.data(),
    static_cast<DWORD>(text.size()),
    &bytes_written,
    nullptr);
}

int appcontainer_file_access_child_main() {
  const command_line_args argv;
  if (argv.argc() != 7) {
    probe_child_write_stdout("appcontainer_file_child_bad_argc=1\n");
    return 2;
  }

  HANDLE raw_token = nullptr;
  require_win32(
    OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token) != FALSE,
    "OpenProcessToken(appcontainer file child)");
  test_unique_handle token(raw_token);

  DWORD is_appcontainer = 0;
  DWORD returned = 0;
  require_win32(
    GetTokenInformation(
      token.get(),
      TokenIsAppContainer,
      &is_appcontainer,
      sizeof(is_appcontainer),
      &returned) != FALSE,
    "GetTokenInformation(TokenIsAppContainer file child)");

  const auto allowed_read = probe_child_can_read_file(std::wstring(argv.at(2)));
  const auto denied_read = !probe_child_can_read_file(std::wstring(argv.at(3)));
  const auto allowed_write = probe_child_can_write_file(std::wstring(argv.at(4)));
  const auto denied_write = !probe_child_can_write_file(std::wstring(argv.at(5)));
  const auto parent_traversal_denied =
    !probe_child_can_read_file(std::wstring(argv.at(6)));

  std::cout << "is_appcontainer=" << (is_appcontainer != 0 ? "1" : "0") << "\n";
  std::cout << "allowed_read=" << (allowed_read ? "1" : "0") << "\n";
  std::cout << "denied_read=" << (denied_read ? "1" : "0") << "\n";
  std::cout << "allowed_write=" << (allowed_write ? "1" : "0") << "\n";
  std::cout << "denied_write=" << (denied_write ? "1" : "0") << "\n";
  std::cout << "parent_traversal_denied="
            << (parent_traversal_denied ? "1" : "0") << "\n";

  return is_appcontainer != 0 && allowed_read && denied_read && allowed_write &&
         denied_write && parent_traversal_denied
         ? 0
         : 3;
}

int appcontainer_probe_child_main() {
  HANDLE raw_token = nullptr;
  require_win32(
    OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token) != FALSE,
    "OpenProcessToken(appcontainer child)");
  test_unique_handle token(raw_token);

  DWORD is_appcontainer = 0;
  DWORD returned = 0;
  require_win32(
    GetTokenInformation(
      token.get(),
      TokenIsAppContainer,
      &is_appcontainer,
      sizeof(is_appcontainer),
      &returned) != FALSE,
    "GetTokenInformation(TokenIsAppContainer)");

  int connect_error = 0;
  winsock_session winsock;
  if (!winsock.started()) {
    connect_error = WSAGetLastError();
  }
  else {
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
      connect_error = WSAGetLastError();
    }
    else {
      u_long nonblocking = 1;
      ioctlsocket(sock, FIONBIO, &nonblocking);
      sockaddr_in address {};
      address.sin_family = AF_INET;
      address.sin_port = htons(80);
      address.sin_addr.s_addr = inet_addr("1.1.1.1");
      if (connect(sock, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        connect_error = WSAGetLastError();
      }
      closesocket(sock);
    }
  }

  std::cout << "is_appcontainer=" << (is_appcontainer != 0 ? "1" : "0") << "\n";
  std::cout << "connect_error=" << connect_error << "\n";
  std::cout << "network_denied=" << (connect_error == WSAEACCES ? "1" : "0") << "\n";
  return is_appcontainer != 0 ? 0 : 3;
}

int sleeping_python_probe_child_main() {
  std::this_thread::sleep_for(std::chrono::seconds(5));
  return 0;
}

#endif

std::string escape_python_string(std::string value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (const auto ch : value) {
    if (ch == '\\' || ch == '\'') {
      escaped.push_back('\\');
    }
    escaped.push_back(ch);
  }
  return escaped;
}

class recording_backend final : public execution::execution_backend {
public:
  sandbox::sandbox_backend_info info() const override {
    return {
      .name = "recording",
      .isolation = sandbox::isolation_level::controlled_process,
      .features = {
        sandbox::sandbox_feature::environment_allowlist,
        sandbox::sandbox_feature::timeout,
        sandbox::sandbox_feature::stdout_capture,
        sandbox::sandbox_feature::stderr_capture,
      },
    };
  }

  execution::execution_result run(
    const execution::execution_request& request,
    std::stop_token) override {
    ++calls;
    last_request = request;
    execution::execution_result result {
      .exit_code = 0,
      .termination_reason = execution::execution_termination_reason::exited,
      .stdout_text = "ok:" + request.code,
    };
    result.metadata["backend"] = "recording";
    return result;
  }

  int calls { 0 };
  execution::execution_request last_request;
};

class throwing_backend final : public execution::execution_backend {
public:
  sandbox::sandbox_backend_info info() const override {
    return {
      .name = "throwing",
      .isolation = sandbox::isolation_level::controlled_process,
    };
  }

  execution::execution_result run(
    const execution::execution_request&,
    std::stop_token) override {
    throw std::runtime_error("backend failed");
  }
};

void test_policy_denies_disallowed_language() {
  auto backend = std::make_unique<recording_backend>();
  auto* backend_ptr = backend.get();
  execution::execution_policy policy;
  policy.allowed_languages.clear();

  execution::execution_runtime runtime(std::move(backend), policy);

  execution::execution_request request;
  request.code = "print(1)";
  const auto result = runtime.run(request);

  assert(result.termination_reason == execution::execution_termination_reason::policy_denied);
  assert(!result.error_message.empty());
  assert(backend_ptr->calls == 0);
}

void test_runtime_clamps_limits_and_uses_env_allowlist() {
  auto backend = std::make_unique<recording_backend>();
  auto* backend_ptr = backend.get();

  execution::execution_policy policy;
  policy.max_limits.timeout = std::chrono::milliseconds(100);
  policy.max_limits.max_stdout_bytes = 10;
  policy.max_limits.max_stderr_bytes = 20;
  policy.allowed_env = { { "SAFE_ENV", "1" } };

  execution::execution_runtime runtime(std::move(backend), policy);

  execution::execution_request request;
  request.code = "print(1)";
  request.limits.timeout = std::chrono::milliseconds(5000);
  request.limits.max_stdout_bytes = 1000;
  request.limits.max_stderr_bytes = 1000;
  request.env = { { "UNSAFE_ENV", "secret" } };

  const auto result = runtime.run(request);

  assert(result.termination_reason == execution::execution_termination_reason::exited);
  assert(backend_ptr->calls == 1);
  assert(backend_ptr->last_request.limits.timeout == std::chrono::milliseconds(100));
  assert(backend_ptr->last_request.limits.max_stdout_bytes == 10);
  assert(backend_ptr->last_request.limits.max_stderr_bytes == 20);
  assert(backend_ptr->last_request.env.size() == 1);
  assert(backend_ptr->last_request.env.at("SAFE_ENV") == "1");
  assert(!backend_ptr->last_request.env.contains("UNSAFE_ENV"));
}

void test_runtime_clamps_resource_limits_and_audits() {
  auto backend = std::make_unique<recording_backend>();
  auto* backend_ptr = backend.get();
  wuwe::agent::audit::in_memory_audit_sink audit;

  execution::execution_policy policy;
  policy.max_limits.max_process_count = 2;
  policy.max_limits.max_memory_bytes = 1024;
  policy.max_limits.max_cpu_time = std::chrono::milliseconds(50);

  execution::execution_runtime runtime(std::move(backend), policy, &audit);

  execution::execution_request request;
  request.code = "print(1)";
  request.limits.max_process_count = 10;
  request.limits.max_memory_bytes = 2048;
  request.limits.max_cpu_time = std::chrono::milliseconds(500);
  const auto result = runtime.run(request);

  assert(result.termination_reason == execution::execution_termination_reason::exited);
  assert(backend_ptr->calls == 1);
  assert(backend_ptr->last_request.limits.max_process_count == 2);
  assert(backend_ptr->last_request.limits.max_memory_bytes == 1024);
  assert(backend_ptr->last_request.limits.max_cpu_time == std::chrono::milliseconds(50));
  assert(result.metadata.at("max_process_count_clamped") == "true");
  assert(result.metadata.at("max_memory_bytes_clamped") == "true");
  assert(result.metadata.at("max_cpu_time_clamped") == "true");
  const auto events = audit.events();
  assert(events.front().attributes.at("max_process_count_clamped") == "true");
  assert(events.front().attributes.at("max_memory_bytes_clamped") == "true");
  assert(events.front().attributes.at("max_cpu_time_clamped") == "true");
}

void test_policy_denies_invalid_allowed_environment_before_backend() {
  auto backend = std::make_unique<recording_backend>();
  auto* backend_ptr = backend.get();

  execution::execution_policy policy;
  policy.allowed_env = { { "BAD=NAME", "1" } };

  execution::execution_runtime runtime(std::move(backend), policy);

  execution::execution_request request;
  request.code = "print(1)";
  const auto result = runtime.run(request);

  assert(result.termination_reason == execution::execution_termination_reason::policy_denied);
  assert(result.error_message.find("invalid execution environment") != std::string::npos);
  assert(backend_ptr->calls == 0);
}

void test_approval_required_without_service_denies_before_backend() {
  auto backend = std::make_unique<recording_backend>();
  auto* backend_ptr = backend.get();
  wuwe::agent::audit::in_memory_audit_sink audit;

  execution::execution_policy policy;
  policy.allow_network = true;
  policy.require_approval_for_network = true;

  execution::execution_runtime runtime(std::move(backend), policy, &audit);

  execution::execution_request request;
  request.code = "print(1)";
  const auto result = runtime.run(request);

  assert(result.termination_reason == execution::execution_termination_reason::approval_denied);
  assert(backend_ptr->calls == 0);
  assert(audit.events().size() == 2);
}

void test_approval_allows_backend_and_audit_records_completion() {
  auto backend = std::make_unique<recording_backend>();
  auto* backend_ptr = backend.get();
  wuwe::agent::audit::in_memory_audit_sink audit;
  wuwe::agent::approval::allow_all_approval_service approvals;

  execution::execution_policy policy;
  policy.allow_file_write = true;
  policy.require_approval_for_file_write = true;

  execution::execution_runtime runtime(
    std::move(backend),
    policy,
    &audit,
    &approvals);

  execution::execution_request request;
  request.code = "print(1)";
  const auto result = runtime.run(request);

  assert(result.termination_reason == execution::execution_termination_reason::exited);
  assert(backend_ptr->calls == 1);
  const auto events = audit.events();
  assert(events.size() == 4);
  assert(events.back().attributes.at("termination_reason") == "exited");
}

void test_tool_provider_exposes_narrow_schema_and_invokes_runtime() {
  auto backend = std::make_unique<recording_backend>();
  auto* backend_ptr = backend.get();

  execution::execution_policy policy;
  policy.max_limits.timeout = std::chrono::milliseconds(250);
  policy.max_limits.max_code_bytes = 64;
  policy.max_limits.max_stdin_bytes = 128;

  execution::execution_runtime runtime(std::move(backend), policy);
  execution::execution_tool_provider provider(runtime);

  const auto tools = provider.tools();
  assert(tools.size() == 1);
  assert(tools[0].name == "run_python_snippet");
  assert(tools[0].parameters_json_schema.find("allow_network") == std::string::npos);
  assert(tools[0].parameters_json_schema.find("allow_file_write") == std::string::npos);
  assert(tools[0].parameters_json_schema.find("env") == std::string::npos);
  const auto schema = nlohmann::json::parse(tools[0].parameters_json_schema);
  assert(schema.at("additionalProperties") == false);
  assert(schema.at("properties").at("code").at("maxLength") == 64);
  assert(schema.at("properties").at("stdin_text").at("maxLength") == 128);
  assert(schema.at("properties").at("timeout_ms").at("maximum") == 250);

  nlohmann::json args {
    { "code", "print(42)" },
  };
  const auto result = provider.invoke("run_python_snippet", args.dump());

  assert(!result.error_code);
  assert(backend_ptr->calls == 1);
  assert(backend_ptr->last_request.limits.timeout == std::chrono::milliseconds(250));
  assert(backend_ptr->last_request.metadata.at("tool_name") == "run_python_snippet");

  const auto content = nlohmann::json::parse(result.content);
  assert(content.at("termination_reason") == "exited");
  assert(content.at("stdout_text") == "ok:print(42)");
}

void test_tool_provider_rejects_arguments_over_limit_before_parse() {
  auto backend = std::make_unique<recording_backend>();
  auto* backend_ptr = backend.get();
  wuwe::agent::audit::in_memory_audit_sink audit;

  execution::execution_policy policy;
  execution::execution_runtime runtime(std::move(backend), policy, &audit);
  execution::execution_tool_provider provider(
    runtime,
    { .max_arguments_bytes = 4 });

  const auto result = provider.invoke("run_python_snippet", "not json at all");

  assert(result.error_code);
  assert(backend_ptr->calls == 0);
  const auto content = nlohmann::json::parse(result.content);
  assert(content.at("termination_reason") == "policy_denied");
  assert(content.at("metadata").at("denial_kind") == "arguments_limit");
  assert(content.at("metadata").at("max_arguments_bytes") == "4");
  const auto events = audit.events();
  assert(events.size() == 1);
  assert(events[0].name == "arguments_limit");
}

void test_tool_provider_rejects_unknown_arguments_and_audits() {
  auto backend = std::make_unique<recording_backend>();
  auto* backend_ptr = backend.get();
  wuwe::agent::audit::in_memory_audit_sink audit;

  execution::execution_policy policy;
  execution::execution_runtime runtime(std::move(backend), policy, &audit);
  execution::execution_tool_provider provider(runtime);

  nlohmann::json args {
    { "code", "print(1)" },
    { "allow_network", true },
  };
  const auto result = provider.invoke("run_python_snippet", args.dump());

  assert(result.error_code);
  assert(backend_ptr->calls == 0);
  const auto content = nlohmann::json::parse(result.content);
  assert(content.at("termination_reason") == "policy_denied");
  assert(content.at("metadata").at("denial_kind") == "schema_invalid");
  assert(content.at("metadata").at("parse_error").get<std::string>().find(
           "unexpected field") != std::string::npos);
  const auto events = audit.events();
  assert(events.size() == 1);
  assert(events[0].name == "schema_invalid");
}

void test_tool_provider_rejects_timeout_over_schema_limit() {
  auto backend = std::make_unique<recording_backend>();
  auto* backend_ptr = backend.get();
  wuwe::agent::audit::in_memory_audit_sink audit;

  execution::execution_policy policy;
  policy.max_limits.timeout = std::chrono::milliseconds(100);
  execution::execution_runtime runtime(std::move(backend), policy, &audit);
  execution::execution_tool_provider provider(runtime);

  nlohmann::json args {
    { "code", "print(1)" },
    { "timeout_ms", 1000 },
  };
  const auto result = provider.invoke("run_python_snippet", args.dump());

  assert(result.error_code);
  assert(backend_ptr->calls == 0);
  const auto content = nlohmann::json::parse(result.content);
  assert(content.at("termination_reason") == "policy_denied");
  assert(content.at("metadata").at("denial_kind") == "timeout_limit");
  assert(content.at("metadata").at("timeout_ms") == "1000");
  assert(content.at("metadata").at("max_timeout_ms") == "100");
  const auto events = audit.events();
  assert(events.size() == 1);
  assert(events[0].name == "timeout_limit");
}

void test_runtime_audit_records_clamped_limits() {
  auto backend = std::make_unique<recording_backend>();
  wuwe::agent::audit::in_memory_audit_sink audit;

  execution::execution_policy policy;
  policy.max_limits.timeout = std::chrono::milliseconds(100);
  execution::execution_runtime runtime(std::move(backend), policy, &audit);

  execution::execution_request request;
  request.code = "print(1)";
  request.limits.timeout = std::chrono::milliseconds(1000);
  const auto result = runtime.run(request);

  assert(result.termination_reason == execution::execution_termination_reason::exited);
  assert(result.metadata.at("timeout_clamped") == "true");
  assert(result.metadata.at("requested_timeout_ms") == "1000");
  const auto events = audit.events();
  assert(events.front().attributes.at("timeout_clamped") == "true");
}

void test_default_backend_registry_exposes_controlled_process() {
  auto registry = execution::make_default_execution_backend_registry();
  const auto backends = registry.backends();
  assert(backends.size() >= 4);
  assert(backends.at(0).name == "controlled_process");
  assert(backends.at(0).available);
  assert(backends.at(0).enforcement.process_tree_cleanup ==
         sandbox::enforcement_level::enforced);
  assert(backends.at(0).enforcement.filesystem_read_deny ==
         sandbox::enforcement_level::not_enforced);
  assert(backends.at(1).name == "restricted_process");
  assert(!backends.at(1).available);
  assert(backends.at(1).enforcement.filesystem_read_deny ==
         sandbox::enforcement_level::planned);
  assert(backends.at(2).name == "container");
  assert(!backends.at(2).available);
  assert(backends.at(3).name == "wasm");
  assert(!backends.at(3).available);
  auto backend = registry.create("controlled_process");
  assert(backend != nullptr);
  assert(registry.create("missing") == nullptr);
}

void test_backend_registry_selects_only_available_enforced_backends() {
  auto registry = execution::make_default_execution_backend_registry();

  execution::execution_backend_requirements controlled_requirements;
  controlled_requirements.require_timeout = true;
  controlled_requirements.require_process_tree_cleanup = true;
  const auto controlled_name = registry.select_backend_name(controlled_requirements);
  assert(controlled_name.has_value());
  assert(*controlled_name == "controlled_process");
  assert(registry.create_best(controlled_requirements) != nullptr);

  execution::execution_backend_requirements strong_requirements;
  strong_requirements.require_filesystem_read_deny = true;
  strong_requirements.require_filesystem_write_deny = true;
  strong_requirements.require_network_deny = true;
  const auto strong_name = registry.select_backend_name(strong_requirements);
  assert(!strong_name.has_value());
  assert(registry.create_best(strong_requirements) == nullptr);

  const auto restricted = registry.describe("restricted_process");
  assert(restricted.has_value());
  assert(!restricted->available);
  assert(!restricted->unavailable_reason.empty());
  assert(registry.describe("missing") == std::nullopt);
}

void test_planned_backend_descriptors_are_not_executable() {
  auto registry = execution::make_default_execution_backend_registry();
  const auto restricted = registry.describe("restricted_process");
  assert(restricted.has_value());
  assert(!restricted->available);
  assert(restricted->isolation == sandbox::isolation_level::restricted_process);
  assert(restricted->enforcement.filesystem_read_deny ==
         sandbox::enforcement_level::planned);
  assert(!restricted->unavailable_reason.empty());
  assert(registry.create("restricted_process") == nullptr);
  assert(registry.create("container") == nullptr);
  assert(registry.create("wasm") == nullptr);
}

void test_controlled_process_contract_reflects_job_object_config() {
  execution::controlled_process_backend backend({
    .use_job_object = false,
  });
  const auto info = backend.info();

  assert(info.enforcement.process_tree_cleanup ==
         sandbox::enforcement_level::not_enforced);
  assert(info.enforcement.process_count_limit ==
         sandbox::enforcement_level::not_enforced);
  assert(info.enforcement.cpu_time_limit ==
         sandbox::enforcement_level::not_enforced);
  assert(info.enforcement.memory_limit ==
         sandbox::enforcement_level::not_enforced);
  assert(info.enforcement.filesystem_read_deny ==
         sandbox::enforcement_level::not_enforced);
  assert(info.enforcement.network_deny ==
         sandbox::enforcement_level::not_enforced);
}

#ifdef _WIN32
void test_windows_restricted_token_probe_launches_child_with_stdio_and_job() {
  HANDLE raw_current_token = nullptr;
  require_win32(
    OpenProcessToken(
      GetCurrentProcess(),
      TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY | TOKEN_QUERY | TOKEN_ADJUST_DEFAULT |
        TOKEN_ADJUST_SESSIONID,
      &raw_current_token) != FALSE,
    "OpenProcessToken(probe source)");
  test_unique_handle current_token(raw_current_token);

  HANDLE raw_restricted_token = nullptr;
  require_win32(
    CreateRestrictedToken(
      current_token.get(),
      DISABLE_MAX_PRIVILEGE,
      0,
      nullptr,
      0,
      nullptr,
      0,
      nullptr,
      &raw_restricted_token) != FALSE,
    "CreateRestrictedToken(probe child)");
  test_unique_handle restricted_token(raw_restricted_token);
  require_condition(
    token_has_restrictions(restricted_token.get()),
    "probe restricted token should report TokenHasRestrictions");

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
  require_win32(
    CreatePipe(&raw_stdin_read, &raw_stdin_write, &security_attributes, 0) != FALSE,
    "CreatePipe(stdin)");
  require_win32(
    CreatePipe(&raw_stdout_read, &raw_stdout_write, &security_attributes, 0) != FALSE,
    "CreatePipe(stdout)");
  require_win32(
    CreatePipe(&raw_stderr_read, &raw_stderr_write, &security_attributes, 0) != FALSE,
    "CreatePipe(stderr)");

  test_unique_handle stdin_read(raw_stdin_read);
  test_unique_handle stdin_write(raw_stdin_write);
  test_unique_handle stdout_read(raw_stdout_read);
  test_unique_handle stdout_write(raw_stdout_write);
  test_unique_handle stderr_read(raw_stderr_read);
  test_unique_handle stderr_write(raw_stderr_write);

  require_win32(
    SetHandleInformation(stdin_write.get(), HANDLE_FLAG_INHERIT, 0) != FALSE,
    "SetHandleInformation(stdin_write)");
  require_win32(
    SetHandleInformation(stdout_read.get(), HANDLE_FLAG_INHERIT, 0) != FALSE,
    "SetHandleInformation(stdout_read)");
  require_win32(
    SetHandleInformation(stderr_read.get(), HANDLE_FLAG_INHERIT, 0) != FALSE,
    "SetHandleInformation(stderr_read)");

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
  test_process_thread_attribute_list attribute_list;
  require_condition(
    attribute_list.initialize_with_handle_list(
      inherited_handles,
      static_cast<DWORD>(std::size(inherited_handles))),
    "initialize inherited handle list");
  startup_ex.lpAttributeList = attribute_list.get();

  test_unique_handle job(CreateJobObjectW(nullptr, nullptr));
  require_win32(job.valid(), "CreateJobObjectW");
  configure_probe_job(job.get());

  auto command_line = quote_windows_arg(current_test_executable_path()) +
                      L" --wuwe-restricted-token-probe-child";
  PROCESS_INFORMATION process {};
  const auto created = CreateProcessAsUserW(
    restricted_token.get(),
    nullptr,
    command_line.data(),
    nullptr,
    nullptr,
    TRUE,
    CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED,
    nullptr,
    nullptr,
    &startup_ex.StartupInfo,
    &process);
  require_win32(created != FALSE, "CreateProcessAsUserW(probe child)");

  test_unique_handle process_handle(process.hProcess);
  test_unique_handle thread_handle(process.hThread);
  stdin_read.reset();
  stdout_write.reset();
  stderr_write.reset();

  require_win32(
    AssignProcessToJobObject(job.get(), process_handle.get()) != FALSE,
    "AssignProcessToJobObject");
  require_win32(
    ResumeThread(thread_handle.get()) != static_cast<DWORD>(-1),
    "ResumeThread");

  stdin_write.reset();
  auto stdout_future = std::async(
    std::launch::async,
    read_pipe_to_end,
    stdout_read.release());
  auto stderr_future = std::async(
    std::launch::async,
    read_pipe_to_end,
    stderr_read.release());

  const auto wait_result = WaitForSingleObject(process_handle.get(), 5000);
  require_condition(
    wait_result == WAIT_OBJECT_0,
    "restricted token probe child did not exit within 5000 ms");

  DWORD exit_code = 1;
  require_win32(
    GetExitCodeProcess(process_handle.get(), &exit_code) != FALSE,
    "GetExitCodeProcess");
  const auto stdout_text = stdout_future.get();
  const auto stderr_text = stderr_future.get();

  if (exit_code != 0 || !stderr_text.empty()) {
    std::cerr << "restricted token probe child exit_code=" << exit_code << "\n";
    std::cerr << "stdout:\n" << stdout_text << "\n";
    std::cerr << "stderr:\n" << stderr_text << "\n";
  }
  require_condition(exit_code == 0, "restricted token probe child failed");
  require_condition(stderr_text.empty(), "restricted token probe child wrote stderr");
  require_condition(
    stdout_text.find("has_restrictions=1") != std::string::npos,
    "restricted token probe child did not report restrictions");
  require_condition(
    stdout_text.find("session_id=") != std::string::npos,
    "restricted token probe child did not report session id");
}

void test_windows_restricted_token_access_check_enforces_file_boundaries() {
  const auto current_user = current_user_sid();
  const auto builtin_users = well_known_sid(WinBuiltinUsersSid);

  const auto run_dir = make_probe_run_directory("acl-file-boundaries");
  probe_directory_cleanup cleanup(run_dir);
  const auto probe_root = run_dir.parent_path();
  const auto allowed_read_dir = run_dir / "allowed-read";
  const auto allowed_write_dir = run_dir / "allowed-write";
  const auto denied_read_dir = run_dir / "denied-read";
  const auto denied_write_dir = run_dir / "denied-write";

  std::filesystem::create_directories(allowed_read_dir);
  std::filesystem::create_directories(allowed_write_dir);
  std::filesystem::create_directories(denied_read_dir);
  std::filesystem::create_directories(denied_write_dir);

  const auto allowed_read_file = allowed_read_dir / "allowed.txt";
  const auto denied_read_file = denied_read_dir / "denied.txt";
  const auto denied_host_write_file = denied_write_dir / "host-can-write.txt";
  {
    std::ofstream(allowed_read_file, std::ios::binary) << "allowed-read-ok";
    std::ofstream(denied_read_file, std::ios::binary) << "denied-read-secret";
  }

  constexpr DWORD users_read_execute = FILE_GENERIC_READ | FILE_GENERIC_EXECUTE;
  set_probe_directory_dacl(
    probe_root,
    current_user.data(),
    builtin_users.data(),
    users_read_execute);
  set_probe_directory_dacl(
    run_dir,
    current_user.data(),
    builtin_users.data(),
    users_read_execute);
  set_probe_directory_dacl(
    allowed_read_dir,
    current_user.data(),
    builtin_users.data(),
    users_read_execute);
  set_probe_file_dacl(
    allowed_read_file,
    current_user.data(),
    builtin_users.data(),
    GENERIC_READ);
  set_probe_directory_dacl(
    allowed_write_dir,
    current_user.data(),
    builtin_users.data(),
    GENERIC_ALL);
  set_probe_directory_dacl(
    denied_read_dir,
    current_user.data(),
    builtin_users.data(),
    0);
  set_probe_file_dacl(
    denied_read_file,
    current_user.data(),
    builtin_users.data(),
    0);
  set_probe_directory_dacl(
    denied_write_dir,
    current_user.data(),
    builtin_users.data(),
    0);

  {
    std::ifstream denied_read(denied_read_file, std::ios::binary);
    require_condition(
      denied_read.good(),
      "host should be able to read denied file before restricted AccessCheck");
    std::ofstream denied_write(denied_host_write_file, std::ios::binary);
    require_condition(
      static_cast<bool>(denied_write << "host-write-ok"),
      "host should be able to write denied directory before restricted AccessCheck");
  }

  const auto restricted_token = create_builtin_users_restricted_token();
  require_condition(
    token_can_access_path(
      restricted_token.get(),
      allowed_read_file,
      FILE_GENERIC_READ),
    "restricted token should read allowed file");
  require_condition(
    !token_can_access_path(
      restricted_token.get(),
      denied_read_file,
      FILE_GENERIC_READ),
    "restricted token should not read denied file");
  require_condition(
    token_can_access_path(
      restricted_token.get(),
      allowed_write_dir,
      FILE_ADD_FILE),
    "restricted token should write allowed directory");
  require_condition(
    !token_can_access_path(
      restricted_token.get(),
      denied_write_dir,
      FILE_ADD_FILE),
    "restricted token should not write denied directory");
}

void test_windows_appcontainer_child_enforces_file_boundaries() {
  const auto profile_name = appcontainer_profile_name();
  const auto profile_name_w = widen_ascii(profile_name);
  DeleteAppContainerProfile(profile_name_w.c_str());

  PSID raw_appcontainer_sid = nullptr;
  HRESULT profile_result = CreateAppContainerProfile(
    profile_name_w.c_str(),
    L"Wuwe File Boundary Probe",
    L"Wuwe test-only AppContainer file boundary probe",
    nullptr,
    0,
    &raw_appcontainer_sid);
  if (profile_result == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) {
    require_error_code(
      DeleteAppContainerProfile(profile_name_w.c_str()),
      ERROR_SUCCESS,
      "DeleteAppContainerProfile(file existing)");
    profile_result = CreateAppContainerProfile(
      profile_name_w.c_str(),
      L"Wuwe File Boundary Probe",
      L"Wuwe test-only AppContainer file boundary probe",
      nullptr,
      0,
      &raw_appcontainer_sid);
  }
  require_condition(
    SUCCEEDED(profile_result),
    "CreateAppContainerProfile should create a file boundary test profile");
  sid_ptr appcontainer_sid(raw_appcontainer_sid);
  struct profile_cleanup {
    std::wstring name;
    ~profile_cleanup() {
      DeleteAppContainerProfile(name.c_str());
    }
  } cleanup_profile { profile_name_w };

  const auto current_user = current_user_sid();
  const auto builtin_users = well_known_sid(WinBuiltinUsersSid);
  const auto run_dir =
    appcontainer_storage_path(appcontainer_sid.get()) / "file-boundaries";
  std::error_code ignored;
  std::filesystem::remove_all(run_dir, ignored);
  std::filesystem::create_directories(run_dir);
  probe_directory_cleanup cleanup_dir(run_dir);
  const auto child_exe = run_dir / "wuwe-appcontainer-file-probe-child.exe";
  const auto allowed_read_dir = run_dir / "allowed-read";
  const auto allowed_write_dir = run_dir / "allowed-write";
  const auto denied_read_dir = run_dir / "denied-read";
  const auto denied_write_dir = run_dir / "denied-write";

  std::filesystem::create_directories(allowed_read_dir);
  std::filesystem::create_directories(allowed_write_dir);
  std::filesystem::create_directories(denied_read_dir);
  std::filesystem::create_directories(denied_write_dir);

  require_win32(
    CopyFileW(
      current_test_executable_path().c_str(),
      child_exe.wstring().c_str(),
      FALSE) != FALSE,
    "CopyFileW(appcontainer file child)");

  const auto allowed_read_file = allowed_read_dir / "allowed.txt";
  const auto denied_read_file = denied_read_dir / "denied.txt";
  const auto allowed_write_file = allowed_write_dir / "restricted-output.txt";
  const auto denied_write_file = denied_write_dir / "restricted-output.txt";
  const auto traversal_read_file =
    allowed_read_dir / ".." / "denied-read" / "denied.txt";
  {
    std::ofstream(allowed_read_file, std::ios::binary) << "allowed-read-ok";
    std::ofstream(denied_read_file, std::ios::binary) << "denied-read-secret";
  }

  grant_directory_access_to_sid(
    run_dir,
    appcontainer_sid.get(),
    FILE_GENERIC_READ | FILE_GENERIC_EXECUTE);
  grant_file_access_to_sid(
    child_exe,
    appcontainer_sid.get(),
    FILE_GENERIC_READ | FILE_GENERIC_EXECUTE);
  grant_directory_access_to_sid(
    allowed_read_dir,
    appcontainer_sid.get(),
    FILE_GENERIC_READ | FILE_GENERIC_EXECUTE);
  grant_file_access_to_sid(
    allowed_read_file,
    appcontainer_sid.get(),
    GENERIC_READ);
  grant_directory_access_to_sid(
    allowed_write_dir,
    appcontainer_sid.get(),
    GENERIC_ALL);
  set_probe_directory_dacl(
    denied_read_dir,
    current_user.data(),
    builtin_users.data(),
    0);
  set_probe_file_dacl(
    denied_read_file,
    current_user.data(),
    builtin_users.data(),
    0);
  set_probe_directory_dacl(
    denied_write_dir,
    current_user.data(),
    builtin_users.data(),
    0);

  {
    std::ifstream denied_read(denied_read_file, std::ios::binary);
    require_condition(
      denied_read.good(),
      "host should be able to read denied file before AppContainer child probe");
    std::ofstream denied_write(denied_write_file, std::ios::binary);
    require_condition(
      static_cast<bool>(denied_write << "host-write-ok"),
      "host should be able to write denied file before AppContainer child probe");
  }

  const auto capture = run_appcontainer_probe_child(
    child_exe,
    appcontainer_sid.get(),
    {
      L"--wuwe-appcontainer-file-access-child",
      allowed_read_file.wstring(),
      denied_read_file.wstring(),
      allowed_write_file.wstring(),
      denied_write_file.wstring(),
      traversal_read_file.wstring(),
    },
    run_dir);

  if (capture.exit_code != 0 || !capture.stderr_text.empty()) {
    std::cerr << "AppContainer file access child exit_code="
              << capture.exit_code << "\n";
    std::cerr << "stdout:\n" << capture.stdout_text << "\n";
    std::cerr << "stderr:\n" << capture.stderr_text << "\n";
  }
  require_condition(
    capture.exit_code == 0,
    "AppContainer file access child failed");
  require_condition(
    capture.stderr_text.empty(),
    "AppContainer file access child wrote stderr");
  require_condition(
    capture.stdout_text.find("is_appcontainer=1") != std::string::npos,
    "AppContainer file access child did not report AppContainer identity");
  require_condition(
    capture.stdout_text.find("allowed_read=1") != std::string::npos,
    "AppContainer file access child could not read allowed file");
  require_condition(
    capture.stdout_text.find("denied_read=1") != std::string::npos,
    "AppContainer file access child read denied file");
  require_condition(
    capture.stdout_text.find("allowed_write=1") != std::string::npos,
    "AppContainer file access child could not write allowed file");
  require_condition(
    capture.stdout_text.find("denied_write=1") != std::string::npos,
    "AppContainer file access child wrote denied file");
  require_condition(
    capture.stdout_text.find("parent_traversal_denied=1") != std::string::npos,
    "AppContainer file access child escaped with parent traversal");
}

void test_windows_appcontainer_probe_launches_child_with_stdio_and_job() {
  const auto profile_name = appcontainer_profile_name();
  const auto profile_name_w = widen_ascii(profile_name);
  DeleteAppContainerProfile(profile_name_w.c_str());

  PSID raw_appcontainer_sid = nullptr;
  HRESULT profile_result = CreateAppContainerProfile(
    profile_name_w.c_str(),
    L"Wuwe Restricted Probe",
    L"Wuwe test-only AppContainer probe",
    nullptr,
    0,
    &raw_appcontainer_sid);
  if (profile_result == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) {
    require_error_code(
      DeleteAppContainerProfile(profile_name_w.c_str()),
      ERROR_SUCCESS,
      "DeleteAppContainerProfile(existing)");
    profile_result = CreateAppContainerProfile(
      profile_name_w.c_str(),
      L"Wuwe Restricted Probe",
      L"Wuwe test-only AppContainer probe",
      nullptr,
      0,
      &raw_appcontainer_sid);
  }
  require_condition(
    SUCCEEDED(profile_result),
    "CreateAppContainerProfile should create a test profile");
  sid_ptr appcontainer_sid(raw_appcontainer_sid);
  struct profile_cleanup {
    std::wstring name;
    ~profile_cleanup() {
      DeleteAppContainerProfile(name.c_str());
    }
  } cleanup_profile { profile_name_w };

  const auto run_dir = make_probe_run_directory("appcontainer-launch");
  probe_directory_cleanup cleanup_dir(run_dir);
  const auto child_exe = run_dir / "wuwe-appcontainer-probe-child.exe";
  require_win32(
    CopyFileW(
      current_test_executable_path().c_str(),
      child_exe.wstring().c_str(),
      FALSE) != FALSE,
    "CopyFileW(appcontainer child)");
  grant_directory_access_to_sid(
    run_dir,
    appcontainer_sid.get(),
    FILE_GENERIC_READ | FILE_GENERIC_EXECUTE);
  grant_file_access_to_sid(
    child_exe,
    appcontainer_sid.get(),
    FILE_GENERIC_READ | FILE_GENERIC_EXECUTE);

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
  require_win32(
    CreatePipe(&raw_stdin_read, &raw_stdin_write, &security_attributes, 0) != FALSE,
    "CreatePipe(appcontainer stdin)");
  require_win32(
    CreatePipe(&raw_stdout_read, &raw_stdout_write, &security_attributes, 0) != FALSE,
    "CreatePipe(appcontainer stdout)");
  require_win32(
    CreatePipe(&raw_stderr_read, &raw_stderr_write, &security_attributes, 0) != FALSE,
    "CreatePipe(appcontainer stderr)");

  test_unique_handle stdin_read(raw_stdin_read);
  test_unique_handle stdin_write(raw_stdin_write);
  test_unique_handle stdout_read(raw_stdout_read);
  test_unique_handle stdout_write(raw_stdout_write);
  test_unique_handle stderr_read(raw_stderr_read);
  test_unique_handle stderr_write(raw_stderr_write);

  require_win32(
    SetHandleInformation(stdin_write.get(), HANDLE_FLAG_INHERIT, 0) != FALSE,
    "SetHandleInformation(appcontainer stdin_write)");
  require_win32(
    SetHandleInformation(stdout_read.get(), HANDLE_FLAG_INHERIT, 0) != FALSE,
    "SetHandleInformation(appcontainer stdout_read)");
  require_win32(
    SetHandleInformation(stderr_read.get(), HANDLE_FLAG_INHERIT, 0) != FALSE,
    "SetHandleInformation(appcontainer stderr_read)");

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
  capabilities.AppContainerSid = appcontainer_sid.get();
  capabilities.Capabilities = nullptr;
  capabilities.CapabilityCount = 0;
  test_process_thread_attribute_list attribute_list;
  require_condition(attribute_list.initialize(2), "initialize AppContainer attribute list");
  require_condition(
    attribute_list.update_handle_list(
      inherited_handles,
      static_cast<DWORD>(std::size(inherited_handles))),
    "update AppContainer inherited handle list");
  require_condition(
    attribute_list.update_security_capabilities(capabilities),
    "update AppContainer security capabilities");
  startup_ex.lpAttributeList = attribute_list.get();

  test_unique_handle job(CreateJobObjectW(nullptr, nullptr));
  require_win32(job.valid(), "CreateJobObjectW(appcontainer)");
  configure_probe_job(job.get());

  auto command_line = quote_windows_arg(child_exe.wstring()) +
                      L" --wuwe-appcontainer-probe-child";
  PROCESS_INFORMATION process {};
  const auto created = CreateProcessW(
    nullptr,
    command_line.data(),
    nullptr,
    nullptr,
    TRUE,
    CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED,
    nullptr,
    run_dir.wstring().c_str(),
    &startup_ex.StartupInfo,
    &process);
  require_win32(created != FALSE, "CreateProcessW(appcontainer child)");

  test_unique_handle process_handle(process.hProcess);
  test_unique_handle thread_handle(process.hThread);
  stdin_read.reset();
  stdout_write.reset();
  stderr_write.reset();

  require_win32(
    AssignProcessToJobObject(job.get(), process_handle.get()) != FALSE,
    "AssignProcessToJobObject(appcontainer)");
  require_win32(
    ResumeThread(thread_handle.get()) != static_cast<DWORD>(-1),
    "ResumeThread(appcontainer)");

  stdin_write.reset();
  auto stdout_future = std::async(
    std::launch::async,
    read_pipe_to_end,
    stdout_read.release());
  auto stderr_future = std::async(
    std::launch::async,
    read_pipe_to_end,
    stderr_read.release());

  const auto wait_result = WaitForSingleObject(process_handle.get(), 5000);
  require_condition(
    wait_result == WAIT_OBJECT_0,
    "AppContainer probe child did not exit within 5000 ms");

  DWORD exit_code = 1;
  require_win32(
    GetExitCodeProcess(process_handle.get(), &exit_code) != FALSE,
    "GetExitCodeProcess(appcontainer)");
  const auto stdout_text = stdout_future.get();
  const auto stderr_text = stderr_future.get();

  if (exit_code != 0 || !stderr_text.empty()) {
    std::cerr << "AppContainer probe child exit_code=" << exit_code << "\n";
    std::cerr << "stdout:\n" << stdout_text << "\n";
    std::cerr << "stderr:\n" << stderr_text << "\n";
  }
  require_condition(exit_code == 0, "AppContainer probe child failed");
  require_condition(stderr_text.empty(), "AppContainer probe child wrote stderr");
  require_condition(
    stdout_text.find("is_appcontainer=1") != std::string::npos,
    "AppContainer probe child did not report AppContainer identity");
  require_condition(
    stdout_text.find("network_denied=") != std::string::npos,
    "AppContainer probe child did not report network probe status");
}
#endif

void test_path_policy_rejects_prefix_trap() {
  const auto base = std::filesystem::temp_directory_path() / "wuwe-path-base";
  const auto sibling = std::filesystem::temp_directory_path() / "wuwe-path-base-other";
  const auto allowed = base / "child.txt";
  const auto rejected = sibling / "child.txt";

  const auto allowed_result = execution::evaluate_path_boundary(allowed, { base });
  const auto rejected_result = execution::evaluate_path_boundary(rejected, { base });

  assert(allowed_result.allowed);
  assert(!rejected_result.allowed);
}

void test_path_policy_handles_parent_traversal() {
  const auto base = std::filesystem::temp_directory_path() / "wuwe-path-parent";
  const auto allowed = base / "nested" / ".." / "child.txt";
  const auto rejected = base / ".." / "wuwe-path-parent-other" / "child.txt";

  const auto allowed_result = execution::evaluate_path_boundary(allowed, { base });
  const auto rejected_result = execution::evaluate_path_boundary(rejected, { base });

  assert(allowed_result.allowed);
  assert(!rejected_result.allowed);
}

void test_runtime_normalizes_backend_exceptions() {
  auto backend = std::make_unique<throwing_backend>();
  wuwe::agent::audit::in_memory_audit_sink audit;
  execution::execution_policy policy;
  execution::execution_runtime runtime(std::move(backend), policy, &audit);

  execution::execution_request request;
  request.code = "print(1)";
  const auto result = runtime.run(request);

  assert(result.termination_reason == execution::execution_termination_reason::backend_error);
  assert(result.error_message == "backend failed");
  assert(audit.events().back().attributes.at("termination_reason") == "backend_error");
}

void test_policy_denies_code_over_input_limit_before_backend() {
  auto backend = std::make_unique<recording_backend>();
  auto* backend_ptr = backend.get();
  wuwe::agent::audit::in_memory_audit_sink audit;

  execution::execution_policy policy;
  policy.max_limits.max_code_bytes = 4;
  policy.max_limits.max_stdin_bytes = 100;
  policy.max_limits.max_total_input_bytes = 100;
  execution::execution_runtime runtime(std::move(backend), policy, &audit);

  execution::execution_request request;
  request.code = "12345";
  const auto result = runtime.run(request);

  assert(result.termination_reason == execution::execution_termination_reason::policy_denied);
  assert(result.error_message.find("code is too large") != std::string::npos);
  assert(result.metadata.at("code_bytes") == "5");
  assert(result.metadata.at("max_code_bytes") == "4");
  assert(backend_ptr->calls == 0);
  const auto events = audit.events();
  assert(events.size() == 1);
  assert(events[0].name == "input_limit");
  assert(events[0].outcome == wuwe::agent::audit::audit_event_outcome::denied);
  assert(events[0].attributes.at("code_bytes") == "5");
}

void test_policy_denies_stdin_over_input_limit_before_backend() {
  auto backend = std::make_unique<recording_backend>();
  auto* backend_ptr = backend.get();

  execution::execution_policy policy;
  policy.max_limits.max_code_bytes = 100;
  policy.max_limits.max_stdin_bytes = 3;
  policy.max_limits.max_total_input_bytes = 100;
  execution::execution_runtime runtime(std::move(backend), policy);

  execution::execution_request request;
  request.code = "1";
  request.stdin_text = "1234";
  const auto result = runtime.run(request);

  assert(result.termination_reason == execution::execution_termination_reason::policy_denied);
  assert(result.error_message.find("stdin_text is too large") != std::string::npos);
  assert(result.metadata.at("stdin_bytes") == "4");
  assert(result.metadata.at("max_stdin_bytes") == "3");
  assert(backend_ptr->calls == 0);
}

void test_policy_denies_total_input_over_limit_before_backend() {
  auto backend = std::make_unique<recording_backend>();
  auto* backend_ptr = backend.get();

  execution::execution_policy policy;
  policy.max_limits.max_code_bytes = 10;
  policy.max_limits.max_stdin_bytes = 10;
  policy.max_limits.max_total_input_bytes = 7;
  execution::execution_runtime runtime(std::move(backend), policy);

  execution::execution_request request;
  request.code = "1234";
  request.stdin_text = "1234";
  const auto result = runtime.run(request);

  assert(result.termination_reason == execution::execution_termination_reason::policy_denied);
  assert(result.error_message.find("total input is too large") != std::string::npos);
  assert(result.metadata.at("code_bytes") == "4");
  assert(result.metadata.at("stdin_bytes") == "4");
  assert(result.metadata.at("max_total_input_bytes") == "7");
  assert(backend_ptr->calls == 0);
}

void test_policy_allows_input_at_exact_limits() {
  auto backend = std::make_unique<recording_backend>();
  auto* backend_ptr = backend.get();

  execution::execution_policy policy;
  policy.max_limits.max_code_bytes = 4;
  policy.max_limits.max_stdin_bytes = 3;
  policy.max_limits.max_total_input_bytes = 7;
  execution::execution_runtime runtime(std::move(backend), policy);

  execution::execution_request request;
  request.code = "1234";
  request.stdin_text = "123";
  const auto result = runtime.run(request);

  assert(result.termination_reason == execution::execution_termination_reason::exited);
  assert(backend_ptr->calls == 1);
}

void test_tool_provider_returns_clear_input_limit_error() {
  auto backend = std::make_unique<recording_backend>();
  auto* backend_ptr = backend.get();

  execution::execution_policy policy;
  policy.max_limits.max_code_bytes = 4;
  execution::execution_runtime runtime(std::move(backend), policy);
  execution::execution_tool_provider provider(runtime);

  nlohmann::json args {
    { "code", "12345" },
  };
  const auto result = provider.invoke("run_python_snippet", args.dump());

  assert(result.error_code);
  assert(backend_ptr->calls == 0);
  const auto content = nlohmann::json::parse(result.content);
  assert(content.at("termination_reason") == "policy_denied");
  assert(content.at("error_message").get<std::string>().find("code is too large") !=
         std::string::npos);
  assert(content.at("metadata").at("code_bytes") == "5");
  assert(content.at("metadata").at("max_code_bytes") == "4");
}

void test_python_interpreter_probe_reports_not_found() {
  const auto probe = execution::probe_python_interpreter({
    .interpreter = std::filesystem::temp_directory_path() /
                   "wuwe-definitely-not-a-real-python.exe",
    .workdir = std::filesystem::temp_directory_path() / "wuwe-execution-tests",
    .timeout = std::chrono::milliseconds(500),
  });

  assert(probe.status == execution::python_interpreter_status::not_found);
  assert(probe.metadata.at("error_code") == "python_interpreter_not_found");
}

void test_python_interpreter_probe_reports_directory_as_not_executable() {
  const auto probe_dir =
    std::filesystem::temp_directory_path() / "wuwe-python-probe-directory";
  std::filesystem::create_directories(probe_dir);
  const auto probe = execution::probe_python_interpreter({
    .interpreter = probe_dir,
    .workdir = std::filesystem::temp_directory_path() / "wuwe-execution-tests",
    .timeout = std::chrono::milliseconds(500),
  });

  assert(probe.status == execution::python_interpreter_status::not_executable);
  assert(probe.metadata.at("error_code") == "python_interpreter_not_executable");
}

#ifdef _WIN32
void test_python_interpreter_probe_reports_startup_timeout() {
  const auto probe = execution::probe_python_interpreter({
    .interpreter = std::filesystem::path(current_test_executable_path()),
    .workdir = std::filesystem::temp_directory_path() / "wuwe-execution-tests",
    .timeout = std::chrono::milliseconds(50),
  });

  assert(probe.status == execution::python_interpreter_status::startup_timeout);
  assert(probe.metadata.at("error_code") == "python_interpreter_startup_timeout");
  assert(probe.metadata.at("timeout_phase") == "startup");
}
#endif

void test_controlled_process_backend_reports_launch_failure() {
  execution::controlled_process_backend backend({
    .python_interpreter = "definitely-not-a-real-python-interpreter",
    .fallback_workdir =
      std::filesystem::temp_directory_path() / "wuwe-execution-tests",
  });

  execution::execution_request request;
  request.code = "print(1)";
  request.limits.timeout = std::chrono::milliseconds(1000);

  const auto result = backend.run(request, {});
  assert(result.termination_reason == execution::execution_termination_reason::launch_failed);
  assert(!result.error_message.empty());
  assert(result.metadata.at("error_code") == "python_interpreter_not_found");
  assert(result.metadata.at("python_interpreter") ==
         "definitely-not-a-real-python-interpreter");
  assert(result.metadata.at("timeout_phase") == "launch");
  assert(result.metadata.count("launch_error_code") == 1);
  assert(result.metadata.count("launch_error_message") == 1);
}

void test_controlled_process_backend_optional_validation_reports_failure() {
  execution::controlled_process_backend backend({
    .python_interpreter = std::filesystem::temp_directory_path() /
                          "wuwe-missing-validation-python.exe",
    .fallback_workdir =
      std::filesystem::temp_directory_path() / "wuwe-execution-tests",
    .validate_python_on_start = true,
    .python_startup_timeout = std::chrono::milliseconds(500),
  });

  execution::execution_request request;
  request.code = "print(1)";
  request.limits.timeout = std::chrono::milliseconds(1000);

  const auto result = backend.run(request, {});
  assert(result.termination_reason == execution::execution_termination_reason::launch_failed);
  assert(result.metadata.at("error_code") == "python_interpreter_not_found");
  assert(result.metadata.at("python_interpreter_probe_status") == "not_found");
}

#ifdef WUWE_EXECUTION_TEST_PYTHON
void test_python_interpreter_probe_reports_valid_python() {
  const auto probe = execution::probe_python_interpreter({
    .interpreter = WUWE_EXECUTION_TEST_PYTHON,
    .workdir = std::filesystem::temp_directory_path() / "wuwe-execution-tests",
    .timeout = std::chrono::milliseconds(3000),
  });

  assert(probe.status == execution::python_interpreter_status::ok);
  assert(!probe.version.empty());
  assert(!probe.executable.empty());
  assert(probe.metadata.at("error_code") == "ok");
}

void test_controlled_process_backend_runs_python_snippet() {
  execution::controlled_process_backend backend({
    .python_interpreter = WUWE_EXECUTION_TEST_PYTHON,
    .fallback_workdir =
      std::filesystem::temp_directory_path() / "wuwe-execution-tests",
  });

  execution::execution_request request;
  request.code =
    "import sys\n"
    "print('hello')\n"
    "print(sys.stdin.read(), end='')\n";
  request.stdin_text = "stdin-ok";
  request.limits.timeout = std::chrono::milliseconds(3000);

  const auto result = backend.run(request, {});
  assert(result.termination_reason == execution::execution_termination_reason::exited);
  assert(result.exit_code.has_value());
  assert(*result.exit_code == 0);
  assert(result.stdout_text.find("hello") != std::string::npos);
  assert(result.stdout_text.find("stdin-ok") != std::string::npos);
}

void test_controlled_process_backend_times_out_python() {
  execution::controlled_process_backend backend({
    .python_interpreter = WUWE_EXECUTION_TEST_PYTHON,
    .fallback_workdir =
      std::filesystem::temp_directory_path() / "wuwe-execution-tests",
  });

  execution::execution_request request;
  request.code = "while True:\n    pass\n";
  request.limits.timeout = std::chrono::milliseconds(100);

  const auto result = backend.run(request, {});
  assert(result.termination_reason == execution::execution_termination_reason::timeout);
  assert(result.timed_out);
}

void test_controlled_process_backend_truncates_stdout_and_stderr() {
  execution::controlled_process_backend backend({
    .python_interpreter = WUWE_EXECUTION_TEST_PYTHON,
    .fallback_workdir =
      std::filesystem::temp_directory_path() / "wuwe-execution-tests",
  });

  execution::execution_request request;
  request.code =
    "import sys\n"
    "sys.stdout.write('abcdef')\n"
    "sys.stderr.write('uvwxyz')\n";
  request.limits.timeout = std::chrono::milliseconds(3000);
  request.limits.max_stdout_bytes = 3;
  request.limits.max_stderr_bytes = 2;

  const auto result = backend.run(request, {});
  assert(result.termination_reason == execution::execution_termination_reason::exited);
  assert(result.stdout_text == "abc");
  assert(result.stderr_text == "uv");
  assert(result.stdout_truncated);
  assert(result.stderr_truncated);
}

void test_controlled_process_backend_cancels_python() {
  execution::controlled_process_backend backend({
    .python_interpreter = WUWE_EXECUTION_TEST_PYTHON,
    .fallback_workdir =
      std::filesystem::temp_directory_path() / "wuwe-execution-tests",
  });

  execution::execution_request request;
  request.code = "while True:\n    pass\n";
  request.limits.timeout = std::chrono::milliseconds(3000);

  std::stop_source stop_source;
  auto future = std::async(std::launch::async, [&]() {
    return backend.run(request, stop_source.get_token());
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  stop_source.request_stop();

  const auto result = future.get();
  assert(result.termination_reason == execution::execution_termination_reason::cancelled);
  assert(result.cancelled);
}

void test_controlled_process_backend_runs_concurrent_snippets() {
  execution::controlled_process_backend backend({
    .python_interpreter = WUWE_EXECUTION_TEST_PYTHON,
    .fallback_workdir =
      std::filesystem::temp_directory_path() / "wuwe-execution-tests",
  });

  auto run_one = [&](int id) {
    execution::execution_request request;
    request.code = "print('concurrent-" + std::to_string(id) + "')";
    request.limits.timeout = std::chrono::milliseconds(3000);
    return backend.run(request, {});
  };

  auto first = std::async(std::launch::async, run_one, 1);
  auto second = std::async(std::launch::async, run_one, 2);
  const auto first_result = first.get();
  const auto second_result = second.get();

  assert(first_result.termination_reason == execution::execution_termination_reason::exited);
  assert(second_result.termination_reason == execution::execution_termination_reason::exited);
  assert(first_result.stdout_text.find("concurrent-1") != std::string::npos);
  assert(second_result.stdout_text.find("concurrent-2") != std::string::npos);
}

void test_controlled_process_backend_job_kills_child_process_on_timeout() {
  const auto marker = std::filesystem::temp_directory_path() /
                      "wuwe-execution-child-survived.txt";
  std::error_code ignored;
  std::filesystem::remove(marker, ignored);

  execution::controlled_process_backend backend({
    .python_interpreter = WUWE_EXECUTION_TEST_PYTHON,
    .fallback_workdir =
      std::filesystem::temp_directory_path() / "wuwe-execution-tests",
    .use_job_object = true,
  });

  execution::execution_request request;
  const auto marker_text = escape_python_string(marker.string());
  request.code =
    "import subprocess, sys\n"
    "subprocess.Popen([sys.executable, '-c', "
    "\"import time, pathlib; time.sleep(1); "
    "pathlib.Path('" + marker_text + "').write_text('alive')\"])\n"
    "while True:\n"
    "    pass\n";
  request.limits.timeout = std::chrono::milliseconds(100);
  request.limits.max_process_count = 4;

  const auto result = backend.run(request, {});
  assert(result.termination_reason == execution::execution_termination_reason::timeout);
  assert(result.metadata.at("job_object_enabled") == "true");
  assert(result.metadata.at("process_tree_cleanup_enforcement") == "enforced");
  assert(result.metadata.at("process_count_limit_enforcement") == "enforced");
  assert(result.metadata.at("max_process_count") == "4");
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  assert(!std::filesystem::exists(marker));
}

void test_tool_provider_runs_python_through_controlled_backend() {
  execution::execution_policy policy;
  policy.default_workdir =
    std::filesystem::temp_directory_path() / "wuwe-execution-tests";
  policy.max_limits.timeout = std::chrono::milliseconds(3000);

  auto backend = execution::make_controlled_process_backend({
    .python_interpreter = WUWE_EXECUTION_TEST_PYTHON,
    .fallback_workdir =
      std::filesystem::temp_directory_path() / "wuwe-execution-tests",
  });
  execution::execution_runtime runtime(std::move(backend), policy);
  execution::execution_tool_provider provider(runtime);

  nlohmann::json args {
    { "code", "print('tool-python-ok')" },
    { "timeout_ms", 1000 },
  };
  const auto result = provider.invoke("run_python_snippet", args.dump());

  assert(!result.error_code);
  const auto content = nlohmann::json::parse(result.content);
  assert(content.at("termination_reason") == "exited");
  assert(content.at("stdout_text").get<std::string>().find("tool-python-ok") !=
         std::string::npos);
}
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
  if (argc == 2 &&
      std::string_view(argv[1]) == "--wuwe-restricted-token-probe-child") {
    return restricted_token_probe_child_main();
  }
  if (argc == 7 &&
      std::string_view(argv[1]) == "--wuwe-appcontainer-file-access-child") {
    return appcontainer_file_access_child_main();
  }
  if (argc == 2 &&
      std::string_view(argv[1]) == "--wuwe-appcontainer-probe-child") {
    return appcontainer_probe_child_main();
  }
  if (argc == 2) {
    const std::filesystem::path maybe_probe_script(argv[1]);
    if (maybe_probe_script.filename().string().find("wuwe-python-probe-") == 0 &&
        maybe_probe_script.extension() == ".py") {
      return sleeping_python_probe_child_main();
    }
  }
#endif

  try {
    test_policy_denies_disallowed_language();
    test_runtime_clamps_limits_and_uses_env_allowlist();
    test_runtime_clamps_resource_limits_and_audits();
    test_policy_denies_invalid_allowed_environment_before_backend();
    test_approval_required_without_service_denies_before_backend();
    test_approval_allows_backend_and_audit_records_completion();
    test_tool_provider_exposes_narrow_schema_and_invokes_runtime();
    test_tool_provider_rejects_arguments_over_limit_before_parse();
    test_tool_provider_rejects_unknown_arguments_and_audits();
    test_tool_provider_rejects_timeout_over_schema_limit();
    test_runtime_audit_records_clamped_limits();
    test_default_backend_registry_exposes_controlled_process();
    test_backend_registry_selects_only_available_enforced_backends();
    test_planned_backend_descriptors_are_not_executable();
    test_controlled_process_contract_reflects_job_object_config();
#ifdef _WIN32
    test_windows_restricted_token_probe_launches_child_with_stdio_and_job();
    test_windows_restricted_token_access_check_enforces_file_boundaries();
    test_windows_appcontainer_child_enforces_file_boundaries();
    test_windows_appcontainer_probe_launches_child_with_stdio_and_job();
#endif
    test_path_policy_rejects_prefix_trap();
    test_path_policy_handles_parent_traversal();
    test_runtime_normalizes_backend_exceptions();
    test_policy_denies_code_over_input_limit_before_backend();
    test_policy_denies_stdin_over_input_limit_before_backend();
    test_policy_denies_total_input_over_limit_before_backend();
    test_policy_allows_input_at_exact_limits();
    test_tool_provider_returns_clear_input_limit_error();
    test_python_interpreter_probe_reports_not_found();
    test_python_interpreter_probe_reports_directory_as_not_executable();
#ifdef _WIN32
    test_python_interpreter_probe_reports_startup_timeout();
#endif
    test_controlled_process_backend_reports_launch_failure();
    test_controlled_process_backend_optional_validation_reports_failure();
#ifdef WUWE_EXECUTION_TEST_PYTHON
    test_python_interpreter_probe_reports_valid_python();
    test_controlled_process_backend_runs_python_snippet();
    test_controlled_process_backend_times_out_python();
    test_controlled_process_backend_truncates_stdout_and_stderr();
    test_controlled_process_backend_cancels_python();
    test_controlled_process_backend_runs_concurrent_snippets();
    test_controlled_process_backend_job_kills_child_process_on_timeout();
    test_tool_provider_runs_python_through_controlled_backend();
#endif
  }
  catch (const std::exception& error) {
    std::cerr << "execution_tests failed: " << error.what() << "\n";
    return 1;
  }
  return 0;
}
