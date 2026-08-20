#ifndef WUWE_RUNTIME_HOST_IDEMPOTENCY_REPOSITORY_HPP
#define WUWE_RUNTIME_HOST_IDEMPOTENCY_REPOSITORY_HPP

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

namespace wuwe::runtime_host::detail {

enum class idempotency_claim_status {
  acquired,
  replay,
  conflict,
  pending,
  capacity_exceeded,
};

struct idempotency_claim {
  idempotency_claim_status status { idempotency_claim_status::capacity_exceeded };
  nlohmann::json result;
};

class idempotency_repository {
public:
  virtual ~idempotency_repository() = default;

  [[nodiscard]] virtual idempotency_claim claim(
    const std::string& key, const std::string& fingerprint) = 0;
  virtual void complete(const std::string& key, const std::string& fingerprint,
    const nlohmann::json& result) = 0;
  virtual void erase(const std::string& key, const std::string& fingerprint) = 0;
};

[[nodiscard]] std::shared_ptr<idempotency_repository> make_memory_idempotency_repository(
  std::size_t capacity);
[[nodiscard]] std::shared_ptr<idempotency_repository> make_sqlite_idempotency_repository(
  const std::filesystem::path& path, std::size_t capacity);

} // namespace wuwe::runtime_host::detail

#endif // WUWE_RUNTIME_HOST_IDEMPOTENCY_REPOSITORY_HPP
