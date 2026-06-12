#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <wuwe/agent/memory/openai_embedding_model.hpp>
#include <wuwe/wuwe.h>

namespace knowledge = wuwe::agent::knowledge;
namespace memory = wuwe::agent::memory;

std::string env_value(const char* name, std::string fallback = {}) {
#if defined(_MSC_VER)
  char* value {};
  std::size_t size {};
  if (_dupenv_s(&value, &size, name) != 0 || !value) {
    return fallback;
  }
  std::string result(value);
  std::free(value);
  return result;
#else
  const auto* value = std::getenv(name);
  return value ? std::string(value) : std::move(fallback);
#endif
}

struct example_options {
  std::filesystem::path docs_path { "docs" };
  std::string query { "RAG retrieval citations" };
  std::string qdrant_url { env_value("WUWE_QDRANT_URL", "http://localhost:6333") };
  std::string collection { env_value(
    "WUWE_QDRANT_KNOWLEDGE_COLLECTION", "wuwe_knowledge_rag_demo") };
  std::string embedding_base_url { env_value(
    "OPENAI_BASE_URL", env_value("OPENROUTER_BASE_URL", "https://openrouter.ai/api")) };
  std::string embedding_api_key { env_value("OPENAI_API_KEY", env_value("OPENROUTER_API_KEY")) };
  std::string embedding_model { env_value("OPENAI_EMBEDDING_MODEL",
    env_value("OPENROUTER_EMBEDDING_MODEL", "openai/text-embedding-3-small")) };
  std::string chat_model { env_value("OPENROUTER_CHAT_MODEL", "openrouter/auto") };
  std::size_t limit { 5 };
  std::size_t candidate_limit { 24 };
  bool clear_first {};
  bool generate_answer { true };
  bool query_rewrite {};
  bool llm_summary {};
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
  wuwe::print("{}",
    "Usage: knowledge_qdrant_rag_example [--docs PATH] [--query TEXT] [--limit N]\n"
    "                                    [--candidate-limit N]\n"
    "                                    [--qdrant-url URL]\n"
    "                                    [--collection NAME] [--embedding-base-url URL]\n"
    "                                    [--embedding-model NAME] [--chat-model NAME]\n"
    "                                    [--query-rewrite] [--llm-summary]\n"
    "                                    [--answer] [--no-answer] [--clear]\n\n"
    "Environment defaults:\n"
    "  WUWE_QDRANT_URL, WUWE_QDRANT_KNOWLEDGE_COLLECTION\n"
    "  OPENROUTER_API_KEY, OPENROUTER_BASE_URL, OPENROUTER_EMBEDDING_MODEL, OPENROUTER_CHAT_MODEL\n"
    "  OPENAI_API_KEY, OPENAI_BASE_URL, OPENAI_EMBEDDING_MODEL\n");
}

example_options parse_args(int argc, char** argv) {
  example_options options;

  for (int index = 1; index < argc; ++index) {
    const std::string_view arg(argv[index]);
    if (arg == "--help" || arg == "-h") {
      print_usage();
      std::exit(0);
    }
    if (arg == "--clear") {
      options.clear_first = true;
      continue;
    }
    if (arg == "--answer") {
      options.generate_answer = true;
      continue;
    }
    if (arg == "--no-answer") {
      options.generate_answer = false;
      continue;
    }
    if (arg == "--query-rewrite") {
      options.query_rewrite = true;
      continue;
    }
    if (arg == "--llm-summary") {
      options.llm_summary = true;
      continue;
    }

    auto read_text = [&](const char* name) -> std::string {
      if (index + 1 >= argc) {
        wuwe::println("Missing value for {}", name);
        std::exit(2);
      }
      return argv[++index];
    };

    if (arg == "--docs") {
      options.docs_path = read_text("--docs");
    }
    else if (arg == "--query") {
      options.query = read_text("--query");
    }
    else if (arg == "--limit") {
      const auto value = parse_size(read_text("--limit"));
      if (!value) {
        wuwe::println("Invalid numeric value for --limit");
        std::exit(2);
      }
      options.limit = *value;
    }
    else if (arg == "--candidate-limit") {
      const auto value = parse_size(read_text("--candidate-limit"));
      if (!value) {
        wuwe::println("Invalid numeric value for --candidate-limit");
        std::exit(2);
      }
      options.candidate_limit = *value;
    }
    else if (arg == "--qdrant-url") {
      options.qdrant_url = read_text("--qdrant-url");
    }
    else if (arg == "--collection") {
      options.collection = read_text("--collection");
    }
    else if (arg == "--embedding-base-url") {
      options.embedding_base_url = read_text("--embedding-base-url");
    }
    else if (arg == "--embedding-model") {
      options.embedding_model = read_text("--embedding-model");
    }
    else if (arg == "--chat-model") {
      options.chat_model = read_text("--chat-model");
    }
    else {
      wuwe::println("Unknown argument: {}", arg);
      print_usage();
      std::exit(2);
    }
  }

  if (options.query.empty()) {
    wuwe::println("--query must not be empty");
    std::exit(2);
  }
  return options;
}

int main(int argc, char** argv) {
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);

  try {
    const auto options = parse_args(argc, argv);

    auto embedding =
      std::make_shared<memory::openai_embedding_model>(memory::openai_embedding_model_config {
        .base_url = options.embedding_base_url,
        .api_key = options.embedding_api_key,
        .model = options.embedding_model,
      });

    auto store_path =
      std::filesystem::temp_directory_path() / ("wuwe-qdrant-rag-" + options.collection + ".jsonl");
    auto store = std::make_shared<knowledge::file_knowledge_store>(store_path);
    auto index =
      std::make_shared<knowledge::qdrant_knowledge_index>(knowledge::qdrant_knowledge_index_config {
        .base_url = options.qdrant_url,
        .collection_name = options.collection,
        .embedding_provider = "openai-compatible",
        .embedding_model = options.embedding_model,
      });

    auto retriever = std::make_shared<knowledge::knowledge_retriever>(store,
      index,
      embedding,
      knowledge::knowledge_splitter({
        .max_tokens = 600,
        .overlap_tokens = 80,
        .include_document_summary_chunk = true,
      }),
      knowledge::knowledge_indexing_policy {
        .embedding_provider = "openai-compatible",
        .embedding_model = options.embedding_model,
        .index_schema_version = 1,
      });
    retriever->set_reranker(std::make_shared<knowledge::mmr_knowledge_reranker>());
    if (options.query_rewrite) {
      retriever->set_query_rewriter(std::make_shared<knowledge::llm_knowledge_query_rewriter>(
        std::make_shared<wuwe::openai_compatible_llm_client>(wuwe::llm_client_config {
          .base_url = options.embedding_base_url,
          .api_key = options.embedding_api_key,
          .model = options.chat_model,
        }),
        knowledge::llm_knowledge_query_rewriter_config {
          .model = options.chat_model,
          .max_rewrites = 3,
          .temperature = 0.0,
        }));
    }

    if (options.clear_first) {
      try {
        retriever->clear();
      }
      catch (const std::exception& ex) {
        wuwe::println(
          "[WARN] clear requested but existing collection/store could not be cleared: {}",
          ex.what());
      }
    }

    auto chat_client = (options.generate_answer || options.llm_summary)
                         ? std::make_shared<wuwe::openai_compatible_llm_client>(wuwe::llm_client_config {
                             .base_url = options.embedding_base_url,
                             .api_key = options.embedding_api_key,
                             .model = options.chat_model,
                           })
                         : std::shared_ptr<wuwe::openai_compatible_llm_client> {};

    knowledge::knowledge_rag_service service(
      retriever, knowledge::knowledge_document_loader::make_default(), chat_client);
    std::vector<std::shared_ptr<knowledge::knowledge_document_enricher>> enrichers;
    if (options.llm_summary) {
      enrichers.push_back(std::make_shared<knowledge::llm_knowledge_document_enricher>(chat_client,
        knowledge::llm_knowledge_document_enricher_config {
          .model = options.chat_model,
        }));
    }
    const auto upload = service.upload_document(options.docs_path,
      {
        .metadata = { { "collection", "rag-demo" } },
        .enrichers = std::move(enrichers),
      },
      true);
    const auto& ingest = upload.ingest;

    if (!ingest.errors.empty()) {
      wuwe::println("Qdrant RAG example");
      wuwe::println("docs={} ingested={} skipped={} erased_stale={} errors={}",
        upload.documents,
        ingest.ingested,
        ingest.skipped,
        ingest.erased_stale,
        ingest.errors.size());
      wuwe::println("Ingest errors:");
      for (const auto& error : ingest.errors) {
        wuwe::println("- {}", error);
      }
      return 1;
    }

    knowledge::knowledge_policy policy;
    policy.max_results = options.limit;
    policy.candidate_results = (std::max)(options.candidate_limit, options.limit);
    policy.max_context_chars = 6000;
    policy.surrounding_chunks_before = 1;
    policy.surrounding_chunks_after = 1;

    const auto answer_report = service.ask({
      .query = options.query,
      .model = options.chat_model,
      .policy = policy,
      .generate_answer = options.generate_answer,
    });

    knowledge::knowledge_tool_provider tools(*retriever,
      {
        .max_search_results = options.limit,
      });
    const auto tool_result = tools.invoke("search_knowledge",
      std::string("{\"content\":") + nlohmann::json(options.query).dump() +
        ",\"limit\":" + std::to_string(options.limit) + "}");

    wuwe::println("Qdrant RAG example");
    wuwe::println("docs={} ingested={} skipped={} erased_stale={} errors={}",
      upload.documents,
      ingest.ingested,
      ingest.skipped,
      ingest.erased_stale,
      ingest.errors.size());
    wuwe::println("collection={} qdrant={} embedding_model={} chat_model={}",
      options.collection,
      options.qdrant_url,
      options.embedding_model,
      options.generate_answer ? options.chat_model : "(disabled)");
    wuwe::println(
      "upload_ms={} load_ms={} ingest_ms={}", upload.total_ms, upload.load_ms, upload.ingest_ms);
    wuwe::println(
      "retrieved={} first_stage={} rewritten_queries={} retrieve_ms={} answer_ms={} total_ms={}\n",
      answer_report.citations.size(),
      answer_report.trace.first_stage_count,
      answer_report.trace.rewritten_query_count,
      answer_report.retrieve_ms,
      answer_report.answer_ms,
      answer_report.total_ms);

    if (!ingest.errors.empty()) {
      wuwe::println("Ingest errors:");
      for (const auto& error : ingest.errors) {
        wuwe::println("- {}", error);
      }
      wuwe::println("");
    }

    wuwe::println("Context block:\n{}\n", answer_report.context_block);
    if (options.generate_answer) {
      if (answer_report.answer) {
        wuwe::println("Generated answer:\n{}\n", answer_report.answer.content);
        wuwe::println("LLM usage: prompt_tokens={} completion_tokens={} total_tokens={}\n",
          answer_report.answer.usage.prompt_tokens,
          answer_report.answer.usage.completion_tokens,
          answer_report.answer.usage.total_tokens);
      }
      else {
        wuwe::println("Generated answer:\n[ERROR] {}\n", answer_report.answer.error_code.message());
      }
    }
    wuwe::println("Tool search result:\n{}", tool_result.content);
  }
  catch (const std::exception& ex) {
    wuwe::println("[FAIL] {}", ex.what());
    return 1;
  }

  return 0;
}
