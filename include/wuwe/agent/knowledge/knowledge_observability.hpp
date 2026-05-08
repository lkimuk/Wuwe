#ifndef WUWE_AGENT_KNOWLEDGE_OBSERVABILITY_HPP
#define WUWE_AGENT_KNOWLEDGE_OBSERVABILITY_HPP

#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <wuwe/agent/core/observability.hpp>

namespace wuwe::agent::knowledge {

struct knowledge_event {
  std::string trace_id;
  std::string name;
  std::chrono::system_clock::time_point timestamp { std::chrono::system_clock::now() };
  std::map<std::string, std::string> attributes;
};

class knowledge_event_sink {
public:
  virtual ~knowledge_event_sink() = default;

  virtual void publish(const knowledge_event& event) = 0;
};

inline observability::agent_event knowledge_event_to_agent_event(const knowledge_event& event) {
  return {
    .module = "knowledge",
    .name = event.name,
    .trace_id = event.trace_id,
    .timestamp = event.timestamp,
    .attributes = event.attributes,
  };
}

class in_memory_knowledge_event_sink final : public knowledge_event_sink {
public:
  void publish(const knowledge_event& event) override {
    std::scoped_lock lock(mutex_);
    events_.push_back(event);
  }

  std::vector<knowledge_event> events() const {
    std::scoped_lock lock(mutex_);
    return events_;
  }

  void clear() {
    std::scoped_lock lock(mutex_);
    events_.clear();
  }

private:
  mutable std::mutex mutex_;
  std::vector<knowledge_event> events_;
};

class agent_knowledge_event_sink final : public knowledge_event_sink {
public:
  explicit agent_knowledge_event_sink(std::shared_ptr<observability::event_sink> sink)
      : sink_(std::move(sink)) {
  }

  void publish(const knowledge_event& event) override {
    if (sink_) {
      sink_->publish(knowledge_event_to_agent_event(event));
    }
  }

private:
  std::shared_ptr<observability::event_sink> sink_;
};

} // namespace wuwe::agent::knowledge

#endif // WUWE_AGENT_KNOWLEDGE_OBSERVABILITY_HPP
