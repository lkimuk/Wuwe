#ifndef WUWE_AGENT_TRAINING_TRAINING_CONTEXT_HPP
#define WUWE_AGENT_TRAINING_TRAINING_CONTEXT_HPP

#include <chrono>
#include <map>
#include <optional>
#include <stop_token>
#include <string>

namespace wuwe::agent::training {

struct training_context {
  std::string trace_id;
  std::string request_id;
  std::string subject_id;
  std::string tenant_id;
  std::string workspace_id;
  std::stop_token stop_token;
  std::optional<std::chrono::steady_clock::time_point> deadline;
  std::map<std::string, std::string> metadata;

  [[nodiscard]] bool cancellation_requested() const noexcept {
    return stop_token.stop_requested();
  }

  [[nodiscard]] bool deadline_reached() const noexcept {
    return deadline && std::chrono::steady_clock::now() >= *deadline;
  }

  [[nodiscard]] std::chrono::milliseconds remaining_time() const noexcept {
    if (!deadline)
      return std::chrono::milliseconds::max();
    const auto now = std::chrono::steady_clock::now();
    if (now >= *deadline)
      return std::chrono::milliseconds::zero();
    return std::chrono::duration_cast<std::chrono::milliseconds>(*deadline - now);
  }
};

} // namespace wuwe::agent::training

#endif // WUWE_AGENT_TRAINING_TRAINING_CONTEXT_HPP
