#ifndef WUWE_RUNTIME_HOST_LIFECYCLE_HPP
#define WUWE_RUNTIME_HOST_LIFECYCLE_HPP

#include <chrono>
#include <mutex>
#include <stdexcept>
#include <string>

namespace wuwe::runtime_host {

enum class lifecycle_state {
  created,
  configured,
  starting,
  ready,
  draining,
  stopped,
  failed,
};

struct lifecycle_snapshot {
  lifecycle_state state { lifecycle_state::created };
  std::chrono::system_clock::time_point changed_at { std::chrono::system_clock::now() };
  std::string failure;
};

[[nodiscard]] std::string to_string(lifecycle_state state);
[[nodiscard]] bool terminal(lifecycle_state state) noexcept;
[[nodiscard]] bool valid_transition(lifecycle_state from, lifecycle_state to) noexcept;

class lifecycle_error final : public std::logic_error {
public:
  using std::logic_error::logic_error;
};

class lifecycle final {
public:
  lifecycle() = default;

  lifecycle(const lifecycle&) = delete;
  lifecycle& operator=(const lifecycle&) = delete;

  [[nodiscard]] lifecycle_snapshot snapshot() const;
  [[nodiscard]] lifecycle_state state() const;

  void transition(lifecycle_state next);
  void fail(std::string message);

private:
  mutable std::mutex mutex_;
  lifecycle_snapshot snapshot_;
};

} // namespace wuwe::runtime_host

#endif // WUWE_RUNTIME_HOST_LIFECYCLE_HPP
