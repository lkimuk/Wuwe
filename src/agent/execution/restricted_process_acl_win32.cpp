#include "restricted_process_acl_win32.hpp"

#ifdef _WIN32

#include <aclapi.h>

#include <utility>
#include <vector>

namespace wuwe::agent::execution::detail {
namespace {

restricted_acl_grant_result make_acl_result(
  restricted_acl_grant_status status,
  DWORD win32_error = ERROR_SUCCESS,
  std::string detail = {}) {
  return {
    .status = status,
    .win32_error = win32_error,
    .detail = std::move(detail),
  };
}

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

  [[nodiscard]] HANDLE get() const noexcept {
    return handle_;
  }

  [[nodiscard]] bool valid() const noexcept {
    return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
  }

  void reset(HANDLE handle = nullptr) noexcept {
    if (valid()) {
      CloseHandle(handle_);
    }
    handle_ = handle;
  }

private:
  HANDLE handle_ {};
};

restricted_acl_grant_result reject_reparse_point(
  const std::filesystem::path& path) {
  const auto attributes = GetFileAttributesW(path.wstring().c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    return make_acl_result(
      restricted_acl_grant_status::path_not_found,
      GetLastError(),
      path.string());
  }
  if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    return make_acl_result(
      restricted_acl_grant_status::reparse_point_not_allowed,
      ERROR_CANT_ACCESS_FILE,
      path.string());
  }
  return {};
}

restricted_acl_grant_result copy_current_user_sid(std::vector<BYTE>& sid) {
  HANDLE raw_token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token)) {
    return make_acl_result(
      restricted_acl_grant_status::current_user_sid_failed,
      GetLastError(),
      "OpenProcessToken");
  }
  unique_handle token(raw_token);

  DWORD token_user_size = 0;
  GetTokenInformation(token.get(), TokenUser, nullptr, 0, &token_user_size);
  const auto size_error = GetLastError();
  if (size_error != ERROR_INSUFFICIENT_BUFFER) {
    return make_acl_result(
      restricted_acl_grant_status::current_user_sid_failed,
      size_error,
      "GetTokenInformation(TokenUser size)");
  }

  std::vector<BYTE> token_user_storage(token_user_size);
  if (!GetTokenInformation(
        token.get(),
        TokenUser,
        token_user_storage.data(),
        static_cast<DWORD>(token_user_storage.size()),
        &token_user_size)) {
    return make_acl_result(
      restricted_acl_grant_status::current_user_sid_failed,
      GetLastError(),
      "GetTokenInformation(TokenUser)");
  }

  const auto* token_user =
    reinterpret_cast<const TOKEN_USER*>(token_user_storage.data());
  const auto sid_size = GetLengthSid(token_user->User.Sid);
  sid.resize(sid_size);
  if (!CopySid(sid_size, sid.data(), token_user->User.Sid)) {
    return make_acl_result(
      restricted_acl_grant_status::current_user_sid_failed,
      GetLastError(),
      "CopySid(current user)");
  }
  return {};
}

restricted_acl_grant_result make_well_known_sid(
  WELL_KNOWN_SID_TYPE type,
  std::vector<BYTE>& sid) {
  sid.resize(SECURITY_MAX_SID_SIZE);
  DWORD sid_size = static_cast<DWORD>(sid.size());
  if (!CreateWellKnownSid(type, nullptr, sid.data(), &sid_size)) {
    return make_acl_result(
      restricted_acl_grant_status::well_known_sid_failed,
      GetLastError(),
      "CreateWellKnownSid");
  }
  sid.resize(sid_size);
  return {};
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

restricted_acl_grant_result set_protected_dacl(
  const std::filesystem::path& path,
  std::vector<EXPLICIT_ACCESSW> entries) {
  PACL raw_acl = nullptr;
  const auto acl_error = SetEntriesInAclW(
    static_cast<ULONG>(entries.size()),
    entries.data(),
    nullptr,
    &raw_acl);
  if (acl_error != ERROR_SUCCESS) {
    return make_acl_result(
      restricted_acl_grant_status::build_acl_failed,
      acl_error,
      "SetEntriesInAclW");
  }

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
  if (security_error != ERROR_SUCCESS) {
    return make_acl_result(
      restricted_acl_grant_status::set_dacl_failed,
      security_error,
      path.string());
  }
  return {};
}

restricted_acl_grant_result grant_path_access(
  const std::filesystem::path& path,
  PSID sid,
  DWORD access_permissions,
  bool directory) {
  if (sid == nullptr) {
    return make_acl_result(restricted_acl_grant_status::invalid_sid);
  }
  if (path.empty()) {
    return make_acl_result(restricted_acl_grant_status::empty_path);
  }

  std::error_code error;
  if (!std::filesystem::exists(path, error)) {
    return make_acl_result(
      restricted_acl_grant_status::path_not_found,
      error.value(),
      path.string());
  }
  auto result = reject_reparse_point(path);
  if (result.status != restricted_acl_grant_status::ok) {
    return result;
  }

  std::vector<BYTE> current_user;
  result = copy_current_user_sid(current_user);
  if (result.status != restricted_acl_grant_status::ok) {
    return result;
  }

  std::vector<BYTE> local_system;
  result = make_well_known_sid(WinLocalSystemSid, local_system);
  if (result.status != restricted_acl_grant_status::ok) {
    return result;
  }

  std::vector<BYTE> administrators;
  result = make_well_known_sid(WinBuiltinAdministratorsSid, administrators);
  if (result.status != restricted_acl_grant_status::ok) {
    return result;
  }

  constexpr DWORD inherit_to_children =
    CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE;
  std::vector<EXPLICIT_ACCESSW> entries {
    explicit_access_for_sid(current_user.data(), GENERIC_ALL),
    explicit_access_for_sid(local_system.data(), GENERIC_ALL),
    explicit_access_for_sid(administrators.data(), GENERIC_ALL),
    explicit_access_for_sid(sid, access_permissions),
  };
  if (directory) {
    entries.push_back(
      explicit_access_for_sid(current_user.data(), GENERIC_ALL, inherit_to_children));
    entries.push_back(
      explicit_access_for_sid(local_system.data(), GENERIC_ALL, inherit_to_children));
    entries.push_back(
      explicit_access_for_sid(administrators.data(), GENERIC_ALL, inherit_to_children));
    entries.push_back(
      explicit_access_for_sid(sid, access_permissions, inherit_to_children));
  }

  result = set_protected_dacl(path, std::move(entries));
  if (result.status != restricted_acl_grant_status::ok) {
    return result;
  }

  if (directory) {
    result.directories_granted = 1;
  }
  else {
    result.files_granted = 1;
  }
  return result;
}

void merge_counts(
  restricted_acl_grant_result& target,
  const restricted_acl_grant_result& source) {
  target.directories_granted += source.directories_granted;
  target.files_granted += source.files_granted;
}

} // namespace

const char* to_string(restricted_acl_grant_status status) noexcept {
  switch (status) {
    case restricted_acl_grant_status::ok:
      return "ok";
    case restricted_acl_grant_status::invalid_sid:
      return "invalid_sid";
    case restricted_acl_grant_status::empty_path:
      return "empty_path";
    case restricted_acl_grant_status::path_not_found:
      return "path_not_found";
    case restricted_acl_grant_status::current_user_sid_failed:
      return "current_user_sid_failed";
    case restricted_acl_grant_status::well_known_sid_failed:
      return "well_known_sid_failed";
    case restricted_acl_grant_status::build_acl_failed:
      return "build_acl_failed";
    case restricted_acl_grant_status::set_dacl_failed:
      return "set_dacl_failed";
    case restricted_acl_grant_status::iterate_failed:
      return "iterate_failed";
    case restricted_acl_grant_status::reparse_point_not_allowed:
      return "reparse_point_not_allowed";
  }
  return "unknown";
}

restricted_acl_grant_result grant_restricted_file_access(
  const std::filesystem::path& path,
  PSID sid,
  DWORD access_permissions) {
  return grant_path_access(path, sid, access_permissions, false);
}

restricted_acl_grant_result grant_restricted_directory_access(
  const std::filesystem::path& path,
  PSID sid,
  DWORD access_permissions) {
  return grant_path_access(path, sid, access_permissions, true);
}

restricted_acl_grant_result grant_restricted_tree_access(
  const restricted_acl_grant_request& request) {
  auto result = grant_restricted_directory_access(
    request.path,
    request.sid,
    request.directory_access);
  if (result.status != restricted_acl_grant_status::ok) {
    return result;
  }

  std::error_code error;
  std::filesystem::recursive_directory_iterator it(
    request.path,
    std::filesystem::directory_options::skip_permission_denied,
    error);
  if (error) {
    return make_acl_result(
      restricted_acl_grant_status::iterate_failed,
      error.value(),
      request.path.string());
  }

  for (std::filesystem::recursive_directory_iterator end;
       it != end;
       it.increment(error)) {
    if (error) {
      return make_acl_result(
        restricted_acl_grant_status::iterate_failed,
        error.value(),
        request.path.string());
    }

    auto reparse = reject_reparse_point(it->path());
    if (reparse.status != restricted_acl_grant_status::ok) {
      return reparse;
    }

    std::error_code entry_error;
    if (it->is_directory(entry_error)) {
      auto grant = grant_restricted_directory_access(
        it->path(),
        request.sid,
        request.directory_access);
      if (grant.status != restricted_acl_grant_status::ok) {
        return grant;
      }
      merge_counts(result, grant);
      continue;
    }
    if (entry_error) {
      return make_acl_result(
        restricted_acl_grant_status::iterate_failed,
        entry_error.value(),
        it->path().string());
    }

    if (it->is_regular_file(entry_error)) {
      auto grant = grant_restricted_file_access(
        it->path(),
        request.sid,
        request.file_access);
      if (grant.status != restricted_acl_grant_status::ok) {
        return grant;
      }
      merge_counts(result, grant);
    }
    if (entry_error) {
      return make_acl_result(
        restricted_acl_grant_status::iterate_failed,
        entry_error.value(),
        it->path().string());
    }
  }
  return result;
}

} // namespace wuwe::agent::execution::detail

#endif // _WIN32
