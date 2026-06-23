#include "restricted_process_appcontainer_win32.hpp"

#ifdef _WIN32

#include <sddl.h>
#include <userenv.h>

#include <string_view>
#include <utility>

namespace wuwe::agent::execution::detail {
namespace {

std::wstring widen_ascii(std::string_view text) {
  std::wstring result;
  result.reserve(text.size());
  for (const char ch : text) {
    result.push_back(static_cast<unsigned char>(ch));
  }
  return result;
}

restricted_appcontainer_profile_result make_profile_result(
  restricted_appcontainer_profile_status status,
  HRESULT hresult = S_OK,
  DWORD win32_error = ERROR_SUCCESS,
  std::string detail = {}) {
  return {
    .status = status,
    .hresult = hresult,
    .win32_error = win32_error,
    .detail = std::move(detail),
  };
}

bool resolve_appcontainer_storage_path(
  PSID sid,
  std::filesystem::path& storage_path,
  restricted_appcontainer_profile_result& result) {
  LPWSTR raw_sid_text = nullptr;
  if (ConvertSidToStringSidW(sid, &raw_sid_text) == FALSE) {
    result.status = restricted_appcontainer_profile_status::sid_string_failed;
    result.win32_error = GetLastError();
    result.hresult = HRESULT_FROM_WIN32(result.win32_error);
    return false;
  }

  struct sid_text_cleanup {
    LPWSTR text;
    ~sid_text_cleanup() {
      if (text != nullptr) {
        LocalFree(text);
      }
    }
  } cleanup_sid { raw_sid_text };

  PWSTR raw_path = nullptr;
  const auto hresult = GetAppContainerFolderPath(raw_sid_text, &raw_path);
  if (FAILED(hresult)) {
    result.status = restricted_appcontainer_profile_status::storage_path_failed;
    result.hresult = hresult;
    result.win32_error = HRESULT_CODE(hresult);
    return false;
  }

  struct path_cleanup {
    PWSTR path;
    ~path_cleanup() {
      if (path != nullptr) {
        CoTaskMemFree(path);
      }
    }
  } cleanup_path { raw_path };

  storage_path = std::filesystem::path(raw_path);
  return true;
}

} // namespace

restricted_appcontainer_profile::~restricted_appcontainer_profile() {
  reset();
}

restricted_appcontainer_profile::restricted_appcontainer_profile(
  restricted_appcontainer_profile&& other) noexcept
    : name_(std::move(other.name_)),
      sid_(std::exchange(other.sid_, nullptr)),
      storage_path_(std::move(other.storage_path_)) {
}

restricted_appcontainer_profile& restricted_appcontainer_profile::operator=(
  restricted_appcontainer_profile&& other) noexcept {
  if (this != &other) {
    reset();
    name_ = std::move(other.name_);
    sid_ = std::exchange(other.sid_, nullptr);
    storage_path_ = std::move(other.storage_path_);
  }
  return *this;
}

void restricted_appcontainer_profile::reset() noexcept {
  if (!name_.empty()) {
    DeleteAppContainerProfile(name_.c_str());
    name_.clear();
  }
  if (sid_ != nullptr) {
    FreeSid(sid_);
    sid_ = nullptr;
  }
  storage_path_.clear();
}

const char* to_string(
  restricted_appcontainer_profile_status status) noexcept {
  switch (status) {
    case restricted_appcontainer_profile_status::ok:
      return "ok";
    case restricted_appcontainer_profile_status::empty_name:
      return "empty_name";
    case restricted_appcontainer_profile_status::create_failed:
      return "create_failed";
    case restricted_appcontainer_profile_status::delete_existing_failed:
      return "delete_existing_failed";
    case restricted_appcontainer_profile_status::sid_string_failed:
      return "sid_string_failed";
    case restricted_appcontainer_profile_status::storage_path_failed:
      return "storage_path_failed";
  }
  return "unknown";
}

restricted_appcontainer_profile_result create_restricted_appcontainer_profile(
  const restricted_appcontainer_profile_request& request) {
  if (request.name.empty()) {
    return make_profile_result(
      restricted_appcontainer_profile_status::empty_name);
  }

  const auto name = widen_ascii(request.name);
  PSID raw_sid = nullptr;
  auto hresult = CreateAppContainerProfile(
    name.c_str(),
    request.display_name.c_str(),
    request.description.c_str(),
    nullptr,
    0,
    &raw_sid);

  if (hresult == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS) &&
      request.replace_existing) {
    const auto delete_result = DeleteAppContainerProfile(name.c_str());
    if (FAILED(delete_result)) {
      return make_profile_result(
        restricted_appcontainer_profile_status::delete_existing_failed,
        delete_result,
        HRESULT_CODE(delete_result),
        request.name);
    }
    hresult = CreateAppContainerProfile(
      name.c_str(),
      request.display_name.c_str(),
      request.description.c_str(),
      nullptr,
      0,
      &raw_sid);
  }

  if (FAILED(hresult)) {
    return make_profile_result(
      restricted_appcontainer_profile_status::create_failed,
      hresult,
      HRESULT_CODE(hresult),
      request.name);
  }

  restricted_appcontainer_profile profile;
  profile.name_ = name;
  profile.sid_ = raw_sid;

  auto result = restricted_appcontainer_profile_result {
    .status = restricted_appcontainer_profile_status::ok,
    .profile = std::move(profile),
  };
  if (!resolve_appcontainer_storage_path(
        result.profile->sid(),
        result.profile->storage_path_,
        result)) {
    result.profile.reset();
    return result;
  }

  return result;
}

} // namespace wuwe::agent::execution::detail

#endif // _WIN32
