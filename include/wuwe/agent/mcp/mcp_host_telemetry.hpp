#ifndef WUWE_AGENT_MCP_HOST_TELEMETRY_HPP
#define WUWE_AGENT_MCP_HOST_TELEMETRY_HPP

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <wuwe/agent/core/observability.hpp>
#include <wuwe/agent/mcp/mcp_host_runtime.hpp>

namespace wuwe::agent::mcp {

inline nlohmann::json mcp_host_event_to_json(const mcp_host_event& event) {
  const auto timestamp_ms =
    std::chrono::duration_cast<std::chrono::milliseconds>(
      event.timestamp.time_since_epoch()).count();

  return {
    { "sequence", event.sequence },
    { "timestampUnixMillis", timestamp_ms },
    { "type", to_string(event.type) },
    { "serverId", event.server_id },
    { "method", event.method },
    { "state", to_string(event.state) },
    { "error", event.error },
    { "elapsedMillis", event.elapsed.count() },
    { "metadata", event.metadata },
  };
}

class mcp_host_event_sink {
public:
  virtual ~mcp_host_event_sink() = default;

  virtual void publish(const mcp_host_event& event) = 0;
};

inline observability::agent_event mcp_host_event_to_agent_event(const mcp_host_event& event) {
  return {
    .module = "mcp",
    .name = to_string(event.type),
    .trace_id = "mcp-host-" + std::to_string(event.sequence),
    .subject_id = event.server_id,
    .timestamp = event.timestamp,
    .elapsed = event.elapsed,
    .attributes = {
      { "mcp.server_id", event.server_id },
      { "mcp.method", event.method },
      { "mcp.state", to_string(event.state) },
      { "mcp.error", event.error },
    },
  };
}

class in_memory_mcp_host_event_sink final : public mcp_host_event_sink {
public:
  void publish(const mcp_host_event& event) override {
    std::lock_guard lock(mutex_);
    events_.push_back(event);
  }

  std::vector<mcp_host_event> events() const {
    std::lock_guard lock(mutex_);
    return events_;
  }

  void clear() {
    std::lock_guard lock(mutex_);
    events_.clear();
  }

private:
  mutable std::mutex mutex_;
  std::vector<mcp_host_event> events_;
};

class agent_mcp_host_event_sink final : public mcp_host_event_sink {
public:
  explicit agent_mcp_host_event_sink(std::shared_ptr<observability::event_sink> sink)
      : sink_(std::move(sink)) {
  }

  void publish(const mcp_host_event& event) override {
    if (sink_) {
      sink_->publish(mcp_host_event_to_agent_event(event));
    }
  }

private:
  std::shared_ptr<observability::event_sink> sink_;
};

class jsonl_mcp_host_event_sink final : public mcp_host_event_sink {
public:
  explicit jsonl_mcp_host_event_sink(std::filesystem::path path)
      : path_(std::move(path)) {
  }

  void publish(const mcp_host_event& event) override {
    std::lock_guard lock(mutex_);
    std::ofstream output(path_, std::ios::app);
    if (!output) {
      throw std::runtime_error("failed to open MCP host telemetry file: " + path_.string());
    }
    output << mcp_host_event_to_json(event).dump() << '\n';
  }

  const std::filesystem::path& path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
  std::mutex mutex_;
};

class fanout_mcp_host_event_sink final : public mcp_host_event_sink {
public:
  void add_sink(std::shared_ptr<mcp_host_event_sink> sink) {
    if (!sink) {
      return;
    }
    std::lock_guard lock(mutex_);
    sinks_.push_back(std::move(sink));
  }

  void publish(const mcp_host_event& event) override {
    std::vector<std::shared_ptr<mcp_host_event_sink>> sinks;
    {
      std::lock_guard lock(mutex_);
      sinks = sinks_;
    }
    for (const auto& sink : sinks) {
      sink->publish(event);
    }
  }

private:
  std::mutex mutex_;
  std::vector<std::shared_ptr<mcp_host_event_sink>> sinks_;
};

struct otel_mcp_host_span {
  std::string trace_id;
  std::string name;
  std::string server_id;
  std::string method;
  std::chrono::system_clock::time_point timestamp;
  std::chrono::milliseconds elapsed { 0 };
  nlohmann::json attributes = nlohmann::json::object();
};

class otel_mcp_host_event_sink final : public mcp_host_event_sink {
public:
  void publish(const mcp_host_event& event) override {
    std::lock_guard lock(mutex_);
    spans_.push_back({
      .trace_id = "mcp-host-" + std::to_string(event.sequence),
      .name = to_string(event.type),
      .server_id = event.server_id,
      .method = event.method,
      .timestamp = event.timestamp,
      .elapsed = event.elapsed,
      .attributes = {
        { "mcp.server_id", event.server_id },
        { "mcp.method", event.method },
        { "mcp.state", to_string(event.state) },
        { "mcp.error", event.error },
        { "mcp.elapsed_ms", event.elapsed.count() },
        { "mcp.metadata", event.metadata },
      },
    });
  }

  std::vector<otel_mcp_host_span> spans() const {
    std::lock_guard lock(mutex_);
    return spans_;
  }

  void clear() {
    std::lock_guard lock(mutex_);
    spans_.clear();
  }

private:
  mutable std::mutex mutex_;
  std::vector<otel_mcp_host_span> spans_;
};

class prometheus_mcp_host_event_sink final : public mcp_host_event_sink {
public:
  void publish(const mcp_host_event& event) override {
    std::lock_guard lock(mutex_);
    ++event_counts_[{ to_string(event.type), event.server_id }];
    if (event.type == mcp_host_event_type::request_succeeded ||
        event.type == mcp_host_event_type::request_failed ||
        event.type == mcp_host_event_type::health_check_succeeded ||
        event.type == mcp_host_event_type::health_check_failed) {
      elapsed_sums_[{ to_string(event.type), event.server_id }] +=
        static_cast<double>(event.elapsed.count());
    }
  }

  std::string scrape() const {
    std::lock_guard lock(mutex_);
    std::ostringstream output;
    output << "# TYPE wuwe_mcp_host_events_total counter\n";
    for (const auto& [labels, count] : event_counts_) {
      output << "wuwe_mcp_host_events_total{event=\"" << escape_label(labels.first)
             << "\",server=\"" << escape_label(labels.second) << "\"} " << count << '\n';
    }

    output << "# TYPE wuwe_mcp_host_event_elapsed_ms_sum counter\n";
    for (const auto& [labels, sum] : elapsed_sums_) {
      output << "wuwe_mcp_host_event_elapsed_ms_sum{event=\"" << escape_label(labels.first)
             << "\",server=\"" << escape_label(labels.second) << "\"} " << sum << '\n';
    }
    return output.str();
  }

private:
  static std::string escape_label(const std::string& value) {
    std::string output;
    output.reserve(value.size());
    for (const auto ch : value) {
      if (ch == '\\' || ch == '"') {
        output.push_back('\\');
      }
      output.push_back(ch);
    }
    return output;
  }

  mutable std::mutex mutex_;
  std::map<std::pair<std::string, std::string>, std::size_t> event_counts_;
  std::map<std::pair<std::string, std::string>, double> elapsed_sums_;
};

inline void attach_mcp_host_event_sink(
  mcp_host_runtime& runtime,
  std::shared_ptr<mcp_host_event_sink> sink) {
  runtime.set_event_sink([sink = std::move(sink)](const mcp_host_event& event) {
    if (sink) {
      sink->publish(event);
    }
  });
}

} // namespace wuwe::agent::mcp

#endif // WUWE_AGENT_MCP_HOST_TELEMETRY_HPP
