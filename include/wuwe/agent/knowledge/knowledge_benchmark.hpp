#ifndef WUWE_AGENT_KNOWLEDGE_BENCHMARK_HPP
#define WUWE_AGENT_KNOWLEDGE_BENCHMARK_HPP

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <wuwe/agent/knowledge/knowledge_retriever.hpp>

namespace wuwe::agent::knowledge {

struct knowledge_benchmark_case {
  std::string query;
  std::size_t limit { 6 };
};

struct knowledge_benchmark_report {
  std::size_t query_count {};
  double total_ms {};
  double average_ms {};
  double p50_ms {};
  double p95_ms {};
  double p99_ms {};
  double max_ms {};
  std::size_t total_results {};
  std::vector<double> latency_samples_ms;
};

struct knowledge_benchmark_options {
  std::size_t concurrency { 1 };
  bool keep_latency_samples { true };
};

inline knowledge_benchmark_report benchmark_knowledge_retrieval(
  const knowledge_retriever& retriever,
  const std::vector<knowledge_benchmark_case>& cases,
  knowledge_benchmark_options options = {}) {
  using clock = std::chrono::steady_clock;

  knowledge_benchmark_report report;
  report.query_count = cases.size();
  if (cases.empty()) {
    return report;
  }

  std::vector<double> samples(cases.size());
  std::vector<std::size_t> result_counts(cases.size());
  std::atomic<std::size_t> next_index { 0 };

  const auto worker = [&] {
    while (true) {
      const auto index = next_index.fetch_add(1);
      if (index >= cases.size()) {
        return;
      }
      const auto& item = cases[index];

    knowledge_query query;
    query.text = item.query;
    query.limit = item.limit;

    const auto start = clock::now();
    const auto results = retriever.retrieve(std::move(query));
    const auto elapsed =
      static_cast<double>(
        std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - start).count()) /
      1000.0;
      samples[index] = elapsed;
      result_counts[index] = results.size();
    }
  };

  const auto concurrency = (std::max)(std::size_t { 1 }, options.concurrency);
  std::vector<std::thread> threads;
  threads.reserve(concurrency);
  for (std::size_t index = 0; index < concurrency; ++index) {
    threads.emplace_back(worker);
  }
  for (auto& thread : threads) {
    thread.join();
  }

  for (std::size_t index = 0; index < samples.size(); ++index) {
    report.total_ms += samples[index];
    report.max_ms = (std::max)(report.max_ms, samples[index]);
    report.total_results += result_counts[index];
  }

  auto sorted = samples;
  std::sort(sorted.begin(), sorted.end());
  const auto percentile = [&](double percentile_value) {
    if (sorted.empty()) {
      return 0.0;
    }
    const auto position =
      (percentile_value / 100.0) * static_cast<double>(sorted.size() - 1);
    return sorted[static_cast<std::size_t>(position + 0.5)];
  };

  report.average_ms = report.total_ms / static_cast<double>(cases.size());
  report.p50_ms = percentile(50.0);
  report.p95_ms = percentile(95.0);
  report.p99_ms = percentile(99.0);
  if (options.keep_latency_samples) {
    report.latency_samples_ms = std::move(samples);
  }
  return report;
}

inline std::string knowledge_benchmark_report_to_json(
  const knowledge_benchmark_report& report) {
  std::ostringstream output;
  output << "{";
  output << "\"query_count\":" << report.query_count << ",";
  output << "\"total_ms\":" << report.total_ms << ",";
  output << "\"average_ms\":" << report.average_ms << ",";
  output << "\"p50_ms\":" << report.p50_ms << ",";
  output << "\"p95_ms\":" << report.p95_ms << ",";
  output << "\"p99_ms\":" << report.p99_ms << ",";
  output << "\"max_ms\":" << report.max_ms << ",";
  output << "\"total_results\":" << report.total_results << ",";
  output << "\"latency_samples_ms\":[";
  for (std::size_t index = 0; index < report.latency_samples_ms.size(); ++index) {
    if (index != 0) {
      output << ",";
    }
    output << report.latency_samples_ms[index];
  }
  output << "]}";
  return output.str();
}

} // namespace wuwe::agent::knowledge

#endif // WUWE_AGENT_KNOWLEDGE_BENCHMARK_HPP
