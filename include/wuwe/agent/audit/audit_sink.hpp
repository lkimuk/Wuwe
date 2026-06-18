#ifndef WUWE_AGENT_AUDIT_AUDIT_SINK_HPP
#define WUWE_AGENT_AUDIT_AUDIT_SINK_HPP

#include <mutex>
#include <vector>

#include <wuwe/agent/audit/audit.hpp>

namespace wuwe::agent::audit {

class audit_sink {
public:
  virtual ~audit_sink() = default;

  virtual void publish(const audit_event& event) = 0;
};

class in_memory_audit_sink final : public audit_sink {
public:
  void publish(const audit_event& event) override {
    std::scoped_lock lock(mutex_);
    events_.push_back(event);
  }

  [[nodiscard]] std::vector<audit_event> events() const {
    std::scoped_lock lock(mutex_);
    return events_;
  }

  void clear() {
    std::scoped_lock lock(mutex_);
    events_.clear();
  }

private:
  mutable std::mutex mutex_;
  std::vector<audit_event> events_;
};

} // namespace wuwe::agent::audit

#endif // WUWE_AGENT_AUDIT_AUDIT_SINK_HPP
