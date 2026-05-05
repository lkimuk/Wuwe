#ifndef WUWE_AGENT_KNOWLEDGE_OBSERVABILITY_HPP
#define WUWE_AGENT_KNOWLEDGE_OBSERVABILITY_HPP

#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

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

} // namespace wuwe::agent::knowledge

#endif // WUWE_AGENT_KNOWLEDGE_OBSERVABILITY_HPP
