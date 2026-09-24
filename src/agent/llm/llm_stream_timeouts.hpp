#ifndef WUWE_AGENT_LLM_STREAM_TIMEOUTS_HPP
#define WUWE_AGENT_LLM_STREAM_TIMEOUTS_HPP

#include <wuwe/agent/llm/llm_config.h>
#include <wuwe/net/http_client.h>

#include <chrono>
#include <condition_variable>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

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
  explicit stream_timeout_guard(llm_stream_timeout_options options) : options_(options) {
  }

  std::optional<stream_timeout> check_before_event() const {
    const auto now = std::chrono::steady_clock::now();
    if (!saw_event_ && options_.first_event_ms > 0 &&
        now - started_at_ > std::chrono::milliseconds(options_.first_event_ms)) {
      return stream_timeout { .phase = "first_event", .timeout_ms = options_.first_event_ms };
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

// Signals cancellation even while a synchronous transport is waiting for bytes.
// The transport must honor the supplied stop token. Destruction wakes/joins the
// watchdog immediately; no detached work survives the request.
class stream_deadline_monitor {
public:
  stream_deadline_monitor(llm_stream_timeout_options options, int total_ms)
      : options_(options), total_ms_(total_ms), worker_([this](std::stop_token stop) {
          std::unique_lock lock(mutex_);
          while (!stop.stop_requested()) {
            const auto [deadline, phase] = next_deadline();
            const auto revision = revision_;
            if (changed_.wait_until(lock, stop, deadline, [&] { return revision_ != revision; }))
              continue;
            if (stop.stop_requested())
              return;
            if (std::chrono::steady_clock::now() >= deadline) {
              phase_ = phase;
              lock.unlock();
              cancellation_.request_stop();
              return;
            }
          }
        }) {
  }
  ~stream_deadline_monitor() {
    worker_.request_stop();
    changed_.notify_all();
  }
  std::stop_token token() const noexcept {
    return cancellation_.get_token();
  }
  std::string phase() const {
    std::lock_guard lock(mutex_);
    if (!phase_.empty())
      return phase_;
    const auto [deadline, name] = next_deadline();
    return std::chrono::steady_clock::now() >= deadline ? name : std::string {};
  }
  void mark_event() {
    std::lock_guard lock(mutex_);
    const auto [deadline, phase] = next_deadline();
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      phase_ = phase;
      return;
    }
    seen_ = true;
    last_ = now;
    ++revision_;
    changed_.notify_all();
  }

private:
  std::pair<std::chrono::steady_clock::time_point, std::string> next_deadline() const {
    auto deadline = started_ + std::chrono::milliseconds(total_ms_);
    std::string phase = "total";
    const auto duration = seen_ ? options_.idle_ms : options_.first_event_ms;
    const auto phase_deadline = (seen_ ? last_ : started_) + std::chrono::milliseconds(duration);
    if (duration > 0 && phase_deadline < deadline) {
      deadline = phase_deadline;
      phase = seen_ ? "idle" : "first_event";
    }
    return { deadline, phase };
  }
  llm_stream_timeout_options options_;
  int total_ms_;
  mutable std::mutex mutex_;
  std::condition_variable_any changed_;
  std::stop_source cancellation_;
  std::chrono::steady_clock::time_point started_ { std::chrono::steady_clock::now() };
  std::chrono::steady_clock::time_point last_ { started_ };
  bool seen_ { false };
  std::size_t revision_ { 0 };
  std::string phase_;
  std::jthread worker_;
};

} // namespace agent::llm_detail

WUWE_NAMESPACE_END

#endif // WUWE_AGENT_LLM_STREAM_TIMEOUTS_HPP
