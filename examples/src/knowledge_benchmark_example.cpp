#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <wuwe/wuwe.h>

namespace knowledge = wuwe::agent::knowledge;

class benchmark_embedding_model final : public wuwe::agent::memory::embedding_model {
public:
  std::vector<float> embed(std::string_view text) const override {
    const auto value = std::string(text);
    if (value.find("retrieval") != std::string::npos || value.find("RAG") != std::string::npos) {
      return { 1.0F, 0.0F, 0.0F };
    }
    if (value.find("policy") != std::string::npos || value.find("security") != std::string::npos) {
      return { 0.0F, 1.0F, 0.0F };
    }
    return { 0.0F, 0.0F, 1.0F };
  }
};

std::size_t env_size(const char* name, std::size_t fallback) {
#if defined(_MSC_VER)
  char* value {};
  std::size_t size {};
  if (_dupenv_s(&value, &size, name) != 0 || !value) {
    return fallback;
  }
  std::string text(value);
  std::free(value);
#else
  const auto* value = std::getenv(name);
  if (!value) {
    return fallback;
  }
  std::string text(value);
#endif
  try {
    return static_cast<std::size_t>(std::stoull(text));
  }
  catch (...) {
    return fallback;
  }
}

bool env_enabled(const char* name) {
#if defined(_MSC_VER)
  char* value {};
  std::size_t size {};
  if (_dupenv_s(&value, &size, name) != 0 || !value) {
    return false;
  }
  std::string text(value);
  std::free(value);
#else
  const auto* value = std::getenv(name);
  if (!value) {
    return false;
  }
  std::string text(value);
#endif
  return text != "0" && text != "false";
}

struct benchmark_options {
  std::size_t document_count { env_size("WUWE_KNOWLEDGE_BENCH_DOCS", 1000) };
  std::size_t query_count { env_size("WUWE_KNOWLEDGE_BENCH_QUERIES", 100) };
  std::size_t concurrency { env_size("WUWE_KNOWLEDGE_BENCH_CONCURRENCY", 1) };
  bool json_output { env_enabled("WUWE_KNOWLEDGE_BENCH_JSON") };
};

std::optional<std::size_t> parse_size(std::string_view value) {
  try {
    return static_cast<std::size_t>(std::stoull(std::string(value)));
  }
  catch (...) {
    return std::nullopt;
  }
}

void print_usage() {
  std::cout << "Usage: knowledge_benchmark_example [--docs N] [--queries N] "
               "[--concurrency N] [--json]\n";
}

benchmark_options parse_args(int argc, char** argv) {
  benchmark_options options;
  for (int index = 1; index < argc; ++index) {
    const std::string_view arg(argv[index]);
    if (arg == "--help" || arg == "-h") {
      print_usage();
      std::exit(0);
    }
    if (arg == "--json") {
      options.json_output = true;
      continue;
    }

    auto read_value = [&](const char* name) -> std::optional<std::size_t> {
      if (index + 1 >= argc) {
        std::cerr << "Missing value for " << name << '\n';
        std::exit(2);
      }
      auto value = parse_size(argv[++index]);
      if (!value) {
        std::cerr << "Invalid numeric value for " << name << '\n';
        std::exit(2);
      }
      return value;
    };

    if (arg == "--docs") {
      options.document_count = *read_value("--docs");
    }
    else if (arg == "--queries") {
      options.query_count = *read_value("--queries");
    }
    else if (arg == "--concurrency") {
      options.concurrency = *read_value("--concurrency");
    }
    else {
      std::cerr << "Unknown argument: " << arg << '\n';
      print_usage();
      std::exit(2);
    }
  }
  return options;
}

int main(int argc, char** argv) {
  const auto options = parse_args(argc, argv);

  auto retriever = std::make_shared<knowledge::knowledge_retriever>(
    std::make_shared<knowledge::in_memory_knowledge_store>(),
    std::make_shared<knowledge::in_memory_knowledge_index>(),
    std::make_shared<benchmark_embedding_model>(),
    knowledge::knowledge_splitter({
      .max_chars = 320,
      .overlap_chars = 32,
    }));

  std::vector<knowledge::knowledge_document> documents;
  documents.reserve(options.document_count);
  for (std::size_t index = 0; index < options.document_count; ++index) {
    const auto topic = index % 3;
    const auto content = topic == 0
                           ? "RAG retrieval chunking indexing citations document "
                           : topic == 1
                               ? "security policy tenant access control document "
                               : "operations runbook timeout backoff retry document ";
    documents.push_back({
      .id = "bench-doc-" + std::to_string(index),
      .title = "Benchmark " + std::to_string(index),
      .content = content + std::to_string(index),
      .source_uri = "bench://" + std::to_string(index),
    });
  }

  const auto ingest_start = std::chrono::steady_clock::now();
  const auto ingest = retriever->ingest_batch(documents);
  const auto ingest_ms =
    static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - ingest_start).count());

  std::vector<knowledge::knowledge_benchmark_case> cases;
  cases.reserve(options.query_count);
  for (std::size_t index = 0; index < options.query_count; ++index) {
    const auto topic = index % 3;
    cases.push_back({
      .query = topic == 0
                 ? "RAG retrieval citations"
                 : topic == 1 ? "tenant security policy" : "timeout retry backoff",
      .limit = 5,
    });
  }

  const auto report = knowledge::benchmark_knowledge_retrieval(*retriever, cases, {
    .concurrency = options.concurrency,
  });

  if (options.json_output) {
    std::cout << knowledge::knowledge_benchmark_report_to_json(report) << '\n';
    return 0;
  }

  std::cout << "Knowledge benchmark\n";
  std::cout << "documents=" << options.document_count << " ingested=" << ingest.ingested
            << " ingest_ms=" << ingest_ms << '\n';
  std::cout << "queries=" << report.query_count
            << " concurrency=" << options.concurrency
            << " total_ms=" << report.total_ms
            << " average_ms=" << report.average_ms
            << " p50_ms=" << report.p50_ms
            << " p95_ms=" << report.p95_ms
            << " p99_ms=" << report.p99_ms
            << " max_ms=" << report.max_ms
            << " total_results=" << report.total_results << '\n';
}
