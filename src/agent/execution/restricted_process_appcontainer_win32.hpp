#ifndef WUWE_AGENT_EXECUTION_RESTRICTED_PROCESS_APPCONTAINER_WIN32_HPP
#define WUWE_AGENT_EXECUTION_RESTRICTED_PROCESS_APPCONTAINER_WIN32_HPP

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <filesystem>
#include <optional>
#include <string>

namespace wuwe::agent::execution::detail {

enum class restricted_appcontainer_profile_status {
  ok,
  empty_name,
  create_failed,
  delete_existing_failed,
  sid_string_failed,
  storage_path_failed,
};

struct restricted_appcontainer_profile_request {
  std::string name;
  std::wstring display_name;
  std::wstring description;
  bool replace_existing { true };
};

class restricted_appcontainer_profile {
public:
  restricted_appcontainer_profile() = default;
  ~restricted_appcontainer_profile();

  restricted_appcontainer_profile(const restricted_appcontainer_profile&) = delete;
  restricted_appcontainer_profile& operator=(
    const restricted_appcontainer_profile&) = delete;

  restricted_appcontainer_profile(restricted_appcontainer_profile&& other) noexcept;
  restricted_appcontainer_profile& operator=(
    restricted_appcontainer_profile&& other) noexcept;

  [[nodiscard]] PSID sid() const noexcept {
    return sid_;
  }

  [[nodiscard]] const std::filesystem::path& storage_path() const noexcept {
    return storage_path_;
  }

  [[nodiscard]] const std::wstring& name() const noexcept {
    return name_;
  }

private:
  friend struct restricted_appcontainer_profile_result;
  friend restricted_appcontainer_profile_result create_restricted_appcontainer_profile(
    const restricted_appcontainer_profile_request& request);

  void reset() noexcept;

  std::wstring name_;
  PSID sid_ {};
  std::filesystem::path storage_path_;
};

struct restricted_appcontainer_profile_result {
  restricted_appcontainer_profile_status status {
    restricted_appcontainer_profile_status::ok
  };
  std::optional<restricted_appcontainer_profile> profile;
  HRESULT hresult { S_OK };
  DWORD win32_error { ERROR_SUCCESS };
  std::string detail;
};

[[nodiscard]] const char* to_string(
  restricted_appcontainer_profile_status status) noexcept;

[[nodiscard]] restricted_appcontainer_profile_result
create_restricted_appcontainer_profile(
  const restricted_appcontainer_profile_request& request);

} // namespace wuwe::agent::execution::detail

#endif // _WIN32

#endif // WUWE_AGENT_EXECUTION_RESTRICTED_PROCESS_APPCONTAINER_WIN32_HPP
