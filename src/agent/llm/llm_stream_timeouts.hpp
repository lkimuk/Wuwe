#ifndef WUWE_AGENT_LLM_STREAM_TIMEOUTS_HPP
#define WUWE_AGENT_LLM_STREAM_TIMEOUTS_HPP

#include <wuwe/agent/llm/llm_config.h>
#include <wuwe/net/http_client.h>

#include <chrono>
#include <initializer_list>
#include <optional>
#include <string>

WUWE_NAMESPACE_BEGIN

namespace agent::llm_detail {

struct stream_timeout {
  std::string phase;
  int timeout_ms {};
};

inline int positive_or_zero(int value) {
  return value > 0 ? value : 0;
}

inline int first_positive(std::initializer_list<int> values) {
  for (const auto value : values) {
    if (value > 0) {
      return value;
    }
  }
  return 0;
}

inline http_timeout_options make_stream_http_timeouts(const llm_client_config& config) {
  const auto& stream = config.stream_timeouts;
  return {
    .total_ms = first_positive({ stream.total_ms, config.timeout }),
    .connect_ms = positive_or_zero(stream.connect_ms),
    .read_ms = first_positive({ stream.idle_ms, stream.first_event_ms }),
  };
}

class stream_timeout_guard {
public:
  explicit stream_timeout_guard(llm_stream_timeout_options options)
      : options_(options) {
  }

  std::optional<stream_timeout> check_before_event() const {
    const auto now = std::chrono::steady_clock::now();
    if (!saw_event_ && options_.first_event_ms > 0 &&
        now - started_at_ > std::chrono::milliseconds(options_.first_event_ms)) {
      return stream_timeout { .phase = "first_event",
        .timeout_ms = options_.first_event_ms };
    }
    if (saw_event_ && options_.idle_ms > 0 &&
        now - last_event_at_ > std::chrono::milliseconds(options_.idle_ms)) {
      return stream_timeout { .phase = "idle", .timeout_ms = options_.idle_ms };
    }
    return std::nullopt;
  }

  void mark_event() {
    saw_event_ = true;
    last_event_at_ = std::chrono::steady_clock::now();
  }

private:
  llm_stream_timeout_options options_;
  std::chrono::steady_clock::time_point started_at_ { std::chrono::steady_clock::now() };
  std::chrono::steady_clock::time_point last_event_at_ { started_at_ };
  bool saw_event_ { false };
};

} // namespace agent::llm_detail

WUWE_NAMESPACE_END

#endif // WUWE_AGENT_LLM_STREAM_TIMEOUTS_HPP
