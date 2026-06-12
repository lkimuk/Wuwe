#ifndef WUWE_AGENT_LLM_RETRY_HPP
#define WUWE_AGENT_LLM_RETRY_HPP

#include <algorithm>
#include <chrono>
#include <stop_token>
#include <system_error>
#include <thread>

#include <wuwe/agent/llm/llm_error.h>
#include <wuwe/net/net_errc.h>

WUWE_NAMESPACE_BEGIN

namespace agent::llm_detail {

inline bool is_retryable_error(const std::error_code& ec) {
  return ec == llm_error_code::rate_limited || ec == llm_error_code::timeout ||
         ec == net_errc::rate_limited || ec == net_errc::timeout ||
         ec == net_errc::connection_failed || ec == net_errc::transport_failed ||
         ec == net_errc::server_error || ec == net_errc::service_unavailable;
}

inline int compute_backoff_ms(int attempt, int base_backoff_ms) {
  constexpr int max_power = 6;
  const int clamped_attempt = attempt < max_power ? attempt : max_power;
  return base_backoff_ms * (1 << clamped_attempt);
}

inline bool wait_for_retry(std::stop_token stop_token, std::chrono::milliseconds duration) {
  constexpr auto poll_interval = std::chrono::milliseconds(50);
  auto remaining = duration;
  while (remaining.count() > 0) {
    if (stop_token.stop_requested()) {
      return false;
    }
    const auto sleep_time = (std::min)(remaining, poll_interval);
    std::this_thread::sleep_for(sleep_time);
    remaining -= sleep_time;
  }
  return !stop_token.stop_requested();
}

} // namespace agent::llm_detail

WUWE_NAMESPACE_END

#endif // WUWE_AGENT_LLM_RETRY_HPP
