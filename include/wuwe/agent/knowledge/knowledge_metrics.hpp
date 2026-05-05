#ifndef WUWE_AGENT_KNOWLEDGE_METRICS_HPP
#define WUWE_AGENT_KNOWLEDGE_METRICS_HPP

#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <wuwe/agent/knowledge/knowledge_observability.hpp>

namespace wuwe::agent::knowledge {

class prometheus_knowledge_event_sink final : public knowledge_event_sink {
public:
  void publish(const knowledge_event& event) override {
    std::scoped_lock lock(mutex_);
    ++event_counts_[event.name];

    if (const auto total_ms = event.attributes.find("total_ms");
        total_ms != event.attributes.end()) {
      latency_sums_[event.name] += parse_double(total_ms->second);
    }
    if (const auto final_count = event.attributes.find("final_count");
        final_count != event.attributes.end()) {
      result_count_sums_[event.name] += parse_double(final_count->second);
    }
  }

  std::string scrape() const {
    std::scoped_lock lock(mutex_);
    std::ostringstream output;
    output << "# TYPE wuwe_knowledge_events_total counter\n";
    for (const auto& [name, count] : event_counts_) {
      output << "wuwe_knowledge_events_total{event=\"" << escape_label(name)
             << "\"} " << count << '\n';
    }

    output << "# TYPE wuwe_knowledge_event_latency_ms_sum counter\n";
    for (const auto& [name, sum] : latency_sums_) {
      output << "wuwe_knowledge_event_latency_ms_sum{event=\"" << escape_label(name)
             << "\"} " << sum << '\n';
    }

    output << "# TYPE wuwe_knowledge_retrieval_results_sum counter\n";
    for (const auto& [name, sum] : result_count_sums_) {
      output << "wuwe_knowledge_retrieval_results_sum{event=\"" << escape_label(name)
             << "\"} " << sum << '\n';
    }
    return output.str();
  }

private:
  static double parse_double(const std::string& value) {
    try {
      return std::stod(value);
    }
    catch (...) {
      return 0.0;
    }
  }

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
  std::map<std::string, std::size_t> event_counts_;
  std::map<std::string, double> latency_sums_;
  std::map<std::string, double> result_count_sums_;
};

struct otel_knowledge_span {
  std::string trace_id;
  std::string name;
  std::map<std::string, std::string> attributes;
};

class otel_knowledge_event_sink final : public knowledge_event_sink {
public:
  void publish(const knowledge_event& event) override {
    std::scoped_lock lock(mutex_);
    spans_.push_back({
      .trace_id = event.trace_id,
      .name = event.name,
      .attributes = event.attributes,
    });
  }

  std::vector<otel_knowledge_span> spans() const {
    std::scoped_lock lock(mutex_);
    return spans_;
  }

private:
  mutable std::mutex mutex_;
  std::vector<otel_knowledge_span> spans_;
};

} // namespace wuwe::agent::knowledge

#endif // WUWE_AGENT_KNOWLEDGE_METRICS_HPP
