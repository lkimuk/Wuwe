#include <wuwe/agent/auth/oauth_credential_store.h>

#include <cstdint>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
// Windows types must precede the security/crypto headers.
// clang-format off
#include <windows.h>
#include <sddl.h>
#include <wincrypt.h>
// clang-format on
#endif

WUWE_NAMESPACE_BEGIN
namespace {
class memory_store final : public oauth_credential_store {
  std::vector<oauth_account_record> records_;

public:
  oauth_store_result load() override {
    return { records_, {} };
  }
  std::error_code save(const std::vector<oauth_account_record>& records) override {
    auto next = records;
    records_.swap(next);
    return {};
  }
};

#ifdef _WIN32
constexpr std::size_t vault_limit = 8 * 1024 * 1024;
using json = nlohmann::json;

struct win_handle {
  HANDLE value { INVALID_HANDLE_VALUE };
  win_handle() = default;
  explicit win_handle(HANDLE handle) : value(handle) {
  }
  ~win_handle() {
    if (value != INVALID_HANDLE_VALUE && value != nullptr)
      CloseHandle(value);
  }
  win_handle(const win_handle&) = delete;
  win_handle& operator=(const win_handle&) = delete;
};
struct local_buffer {
  void* value { nullptr };
  DWORD size { 0 };
  ~local_buffer() {
    if (value) {
      SecureZeroMemory(value, size);
      LocalFree(value);
    }
  }
};
struct private_acl {
  local_buffer descriptor;
  SECURITY_ATTRIBUTES attributes { sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE };
  private_acl() {
    win_handle token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value))
      throw std::system_error(make_error_code(oauth_error::storage_unavailable));
    DWORD size = 0;
    GetTokenInformation(token.value, TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> buffer(size);
    if (!GetTokenInformation(token.value, TokenUser, buffer.data(), size, &size))
      throw std::system_error(make_error_code(oauth_error::storage_unavailable));
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &sid))
      throw std::system_error(make_error_code(oauth_error::storage_unavailable));
    local_buffer sid_owner { sid, 0 };
    const std::wstring sddl = L"D:P(A;;FA;;;SY)(A;;FA;;;" + std::wstring(sid) + L")";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          sddl.c_str(), SDDL_REVISION_1, &descriptor.value, nullptr))
      throw std::system_error(make_error_code(oauth_error::storage_unavailable));
    attributes.lpSecurityDescriptor = descriptor.value;
  }
};

class protected_store final : public oauth_credential_store {
  std::filesystem::path path_;
  win_handle lock_;
  private_acl acl_;

public:
  explicit protected_store(const std::filesystem::path& path) : path_(path) {
    if (!path_.is_absolute() || path_.filename().empty())
      throw std::system_error(make_error_code(oauth_error::invalid_argument));
    // The caller creates/owns the directory; never silently choose a new vault.
    const auto lock_path = path_.wstring() + L".lock";
    lock_.value = CreateFileW(lock_path.c_str(),
      GENERIC_READ | GENERIC_WRITE,
      0,
      &acl_.attributes,
      OPEN_ALWAYS,
      FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_DELETE_ON_CLOSE,
      nullptr);
    if (lock_.value == INVALID_HANDLE_VALUE) {
      const auto code = GetLastError();
      throw std::system_error(
        make_error_code(code == ERROR_SHARING_VIOLATION ? oauth_error::storage_in_use
                                                        : oauth_error::storage_unavailable));
    }
  }

  oauth_store_result load() override {
    win_handle file(CreateFileW(path_.c_str(),
      GENERIC_READ,
      FILE_SHARE_READ,
      nullptr,
      OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr));
    if (file.value == INVALID_HANDLE_VALUE) {
      if (GetLastError() == ERROR_FILE_NOT_FOUND)
        return {};
      return { {}, oauth_error::storage_failed };
    }
    BY_HANDLE_FILE_INFORMATION info {};
    LARGE_INTEGER length {};
    if (!GetFileInformationByHandle(file.value, &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        !GetFileSizeEx(file.value, &length) || length.QuadPart <= 0 ||
        length.QuadPart > static_cast<LONGLONG>(vault_limit))
      return { {}, oauth_error::storage_corrupt };
    std::vector<unsigned char> encrypted(static_cast<std::size_t>(length.QuadPart));
    DWORD read = 0;
    if (!ReadFile(
          file.value, encrypted.data(), static_cast<DWORD>(encrypted.size()), &read, nullptr) ||
        read != encrypted.size())
      return { {}, oauth_error::storage_failed };
    DATA_BLOB source { read, encrypted.data() }, decoded {};
    if (!CryptUnprotectData(
          &source, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &decoded))
      return { {}, oauth_error::storage_corrupt };
    local_buffer plaintext { decoded.pbData, decoded.cbData };
    if (decoded.cbData > vault_limit)
      return { {}, oauth_error::storage_corrupt };
    try {
      const auto root = json::parse(decoded.pbData, decoded.pbData + decoded.cbData);
      if (root.at("version") != 1 || !root.at("accounts").is_array() ||
          root.at("accounts").size() > 256)
        return { {}, oauth_error::storage_corrupt };
      oauth_store_result result;
      for (const auto& item : root.at("accounts")) {
        const auto state = item.at("state").get<int>();
        const auto seconds = item.at("expires_at").get<std::int64_t>();
        // Bound before duration conversion to avoid overflowing clock::duration.
        if ((state != 0 && state != 1) || seconds < 0 || seconds > 4'102'444'800LL)
          return { {}, oauth_error::storage_corrupt };
        result.records.push_back(
          { { { item.at("provider").get<std::string>(), item.at("account_id").get<std::string>() },
              item.at("display_name").get<std::string>(),
              item.at("upstream_account_id").get<std::string>(),
              static_cast<oauth_account_state>(state),
              item.value("subject_id", std::string {}) },
            { item.at("access_token").get<std::string>(),
              item.at("refresh_token").get<std::string>(),
              item.at("id_token").get<std::string>(),
              std::chrono::system_clock::time_point(std::chrono::seconds(seconds)) } });
      }
      return result;
    }
    catch (...) {
      return { {}, oauth_error::storage_corrupt };
    }
  }

  std::error_code save(const std::vector<oauth_account_record>& records) override {
    if (records.size() > 256)
      return oauth_error::storage_failed;
    json root { { "version", 1 }, { "accounts", json::array() } };
    for (const auto& record : records) {
      const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
        record.tokens.expires_at.time_since_epoch())
                             .count();
      if (seconds < 0 || seconds > 4'102'444'800LL)
        return oauth_error::storage_failed;
      root["accounts"].push_back({ { "provider", record.account.key.provider },
        { "account_id", record.account.key.account_id },
        { "display_name", record.account.display_name },
        { "upstream_account_id", record.account.upstream_account_id },
        { "subject_id", record.account.subject_id },
        { "state", static_cast<int>(record.account.state) },
        { "access_token", record.tokens.access_token },
        { "refresh_token", record.tokens.refresh_token },
        { "id_token", record.tokens.id_token },
        { "expires_at", seconds } });
    }
    auto plaintext = root.dump();
    if (plaintext.size() > vault_limit - 4096) {
      SecureZeroMemory(plaintext.data(), plaintext.size());
      return oauth_error::storage_failed;
    }
    DATA_BLOB source { static_cast<DWORD>(plaintext.size()),
      reinterpret_cast<BYTE*>(plaintext.data()) },
      encrypted {};
    const auto ok = CryptProtectData(&source,
      L"Wuwe OAuth credentials",
      nullptr,
      nullptr,
      nullptr,
      CRYPTPROTECT_UI_FORBIDDEN,
      &encrypted);
    SecureZeroMemory(plaintext.data(), plaintext.size());
    if (!ok)
      return oauth_error::storage_failed;
    local_buffer ciphertext { encrypted.pbData, encrypted.cbData };
    const auto temp = path_.wstring() + L".pending";
    // Exclusive store ownership makes this fixed sibling safe for recovery.
    // Inspect before truncating a crash-leftover temporary file; never follow
    // reparse points or write through a hard link. Reapply the private ACL even
    // when the file already exists (creation attributes alone do not do that).
    win_handle file(CreateFileW(temp.c_str(),
      GENERIC_WRITE | WRITE_DAC,
      0,
      &acl_.attributes,
      OPEN_ALWAYS,
      FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr));
    if (file.value == INVALID_HANDLE_VALUE)
      return oauth_error::storage_failed;
    BY_HANDLE_FILE_INFORMATION info {};
    DWORD written = 0;
    const bool saved =
      GetFileInformationByHandle(file.value, &info) &&
      !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && info.nNumberOfLinks == 1 &&
      SetKernelObjectSecurity(file.value,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        acl_.descriptor.value) &&
      SetEndOfFile(file.value) &&
      WriteFile(file.value, encrypted.pbData, encrypted.cbData, &written, nullptr) &&
      written == encrypted.cbData && FlushFileBuffers(file.value);
    CloseHandle(file.value);
    file.value = INVALID_HANDLE_VALUE;
    if (!saved) {
      DeleteFileW(temp.c_str());
      return oauth_error::storage_failed;
    }
    if (!MoveFileExW(
          temp.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      DeleteFileW(temp.c_str());
      return oauth_error::storage_failed;
    }
    return {};
  }
};
#endif
} // namespace

std::unique_ptr<oauth_credential_store> make_memory_oauth_credential_store() {
  return std::make_unique<memory_store>();
}
std::unique_ptr<oauth_credential_store> make_system_oauth_credential_store(
  const std::filesystem::path& path) {
#ifdef _WIN32
  return std::make_unique<protected_store>(path);
#else
  (void)path;
  throw std::system_error(make_error_code(oauth_error::storage_unavailable));
#endif
}
WUWE_NAMESPACE_END
