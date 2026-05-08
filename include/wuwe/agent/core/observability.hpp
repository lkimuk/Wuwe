#ifndef WUWE_AGENT_CORE_OBSERVABILITY_HPP
#define WUWE_AGENT_CORE_OBSERVABILITY_HPP

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace wuwe::agent::observability {

struct agent_event {
  std::string module;
  std::string name;
  std::string trace_id;
  std::string subject_id;
  std::chrono::system_clock::time_point timestamp { std::chrono::system_clock::now() };
  std::chrono::milliseconds elapsed { 0 };
  std::map<std::string, std::string> attributes;
};

inline nlohmann::json agent_event_to_json(const agent_event& event) {
  const auto timestamp_ms =
    std::chrono::duration_cast<std::chrono::milliseconds>(
      event.timestamp.time_since_epoch()).count();
  return {
    { "module", event.module },
    { "name", event.name },
    { "traceId", event.trace_id },
    { "subjectId", event.subject_id },
    { "timestampUnixMillis", timestamp_ms },
    { "elapsedMillis", event.elapsed.count() },
    { "attributes", event.attributes },
  };
}

class event_sink {
public:
  virtual ~event_sink() = default;
  virtual void publish(const agent_event& event) = 0;
};

class in_memory_event_sink final : public event_sink {
public:
  void publish(const agent_event& event) override {
    std::scoped_lock lock(mutex_);
    events_.push_back(event);
  }

  std::vector<agent_event> events() const {
    std::scoped_lock lock(mutex_);
    return events_;
  }

  void clear() {
    std::scoped_lock lock(mutex_);
    events_.clear();
  }

private:
  mutable std::mutex mutex_;
  std::vector<agent_event> events_;
};

class fanout_event_sink final : public event_sink {
public:
  void add_sink(std::shared_ptr<event_sink> sink) {
    if (!sink) {
      return;
    }
    std::scoped_lock lock(mutex_);
    sinks_.push_back(std::move(sink));
  }

  void publish(const agent_event& event) override {
    std::vector<std::shared_ptr<event_sink>> sinks;
    {
      std::scoped_lock lock(mutex_);
      sinks = sinks_;
    }
    for (const auto& sink : sinks) {
      sink->publish(event);
    }
  }

private:
  mutable std::mutex mutex_;
  std::vector<std::shared_ptr<event_sink>> sinks_;
};

class jsonl_event_sink final : public event_sink {
public:
  explicit jsonl_event_sink(std::filesystem::path path) : path_(std::move(path)) {
  }

  void publish(const agent_event& event) override {
    std::scoped_lock lock(mutex_);
    std::ofstream output(path_, std::ios::app);
    if (!output) {
      throw std::runtime_error("failed to open agent event file: " + path_.string());
    }
    output << agent_event_to_json(event).dump() << '\n';
  }

private:
  std::filesystem::path path_;
  std::mutex mutex_;
};

} // namespace wuwe::agent::observability

#endif // WUWE_AGENT_CORE_OBSERVABILITY_HPP
