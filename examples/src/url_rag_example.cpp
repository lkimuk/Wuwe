#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cctype>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "console_utf8.hpp"

#include <nlohmann/json.hpp>

#include <wuwe/agent/knowledge/in_memory_knowledge_index.hpp>
#include <wuwe/agent/knowledge/in_memory_knowledge_store.hpp>
#include <wuwe/agent/knowledge/knowledge_context.hpp>
#include <wuwe/agent/knowledge/knowledge_document_loader.hpp>
#include <wuwe/agent/knowledge/knowledge_rag_service.hpp>
#include <wuwe/agent/knowledge/knowledge_reranker.hpp>
#include <wuwe/agent/knowledge/knowledge_retriever.hpp>
#include <wuwe/agent/knowledge/knowledge_tools.hpp>
#include <wuwe/agent/llm/openai_compatible_llm_client.h>
#include <wuwe/agent/memory/embedding_model.hpp>
#include <wuwe/wuwe.h>

namespace {

namespace knowledge = wuwe::agent::knowledge;
namespace memory = wuwe::agent::memory;

constexpr std::string_view kGuidelinesUrl =
  "https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines";
constexpr std::string_view kQuestion =
  "What do the C++ Core Guidelines recommend when code needs dynamic allocation with new and delete?";
constexpr std::string_view kRetrievalHints =
  "dynamic allocation new delete explicit resource handle manager object R.11 R.12 smart pointer unique_ptr make_unique vector RAII";

struct text {
  static bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
  }

  static bool starts_with(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
  }

  static std::string lower(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
      return static_cast<char>(std::tolower(ch));
    });
    return result;
  }

  static std::string preview(std::string value, std::size_t max_chars = 1600) {
    if (value.size() <= max_chars) {
      return value;
    }
    value.resize(max_chars);
    if (value.size() > 3) {
      value.resize(value.size() - 3);
      value += "...";
    }
    return value;
  }
};

class env_reader {
public:
  static std::string get(const char* name, std::string fallback = {}) {
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
};

class citation_guard {
public:
  static bool answer_uses_context_citations(
    std::string_view answer,
    std::string_view context_block) {
    const auto max_context_citation = max_citation_number(context_block);
    if (max_context_citation == 0) {
      return false;
    }

    bool saw_answer_citation = false;
    bool valid = true;
    for_each_citation(answer, [&](int citation) {
      saw_answer_citation = true;
      if (citation <= 0 || citation > max_context_citation) {
        valid = false;
      }
    });
    return saw_answer_citation && valid;
  }

private:
  static int max_citation_number(std::string_view value) {
    int result = 0;
    for_each_citation(value, [&](int citation) {
      if (citation > result) {
        result = (std::max)(result, citation);
      }
    });
    return result;
  }

  template<typename Callback>
  static void for_each_citation(std::string_view value, Callback callback) {
    for (std::size_t index = 0; index < value.size(); ++index) {
      if (value[index] != '[') {
        continue;
      }
      std::size_t cursor = index + 1;
      int citation = 0;
      bool saw_digit = false;
      while (cursor < value.size() && std::isdigit(static_cast<unsigned char>(value[cursor]))) {
        saw_digit = true;
        citation = citation * 10 + (value[cursor] - '0');
        ++cursor;
      }
      if (saw_digit && cursor < value.size() && value[cursor] == ']') {
        callback(citation);
      }
    }
  }
};

struct guideline_demo_spec {
  static std::string retrieval_query() {
    return std::string(kQuestion) + "\nRetrieval hints: " + std::string(kRetrievalHints);
  }

  static bool context_is_relevant(std::string_view context_block) {
    const auto normalized = text::lower(context_block);
    return contains_all(normalized, required_context_terms());
  }

  static bool answer_is_relevant(std::string_view answer) {
    const auto normalized = text::lower(answer);
    return (text::contains(normalized, "new") || text::contains(normalized, "delete")) &&
           contains_any(normalized, answer_resource_terms());
  }

private:
  static constexpr std::array<std::string_view, 3> required_context_terms() {
    return std::array {
      std::string_view { "r.11: avoid calling new and delete explicitly" },
      std::string_view { "r.12: immediately give the result of an explicit resource allocation" },
      std::string_view { "manager object" },
    };
  }

  static constexpr std::array<std::string_view, 4> answer_resource_terms() {
    return std::array {
      std::string_view { "resource" },
      std::string_view { "raii" },
      std::string_view { "unique_ptr" },
      std::string_view { "manager" },
    };
  }

  template<std::size_t Size>
  static bool contains_all(const std::string& text_value, const std::array<std::string_view, Size>& terms) {
    return std::all_of(terms.begin(), terms.end(), [&](std::string_view term) {
      return text::contains(text_value, term);
    });
  }

  template<std::size_t Size>
  static bool contains_any(const std::string& text_value, const std::array<std::string_view, Size>& terms) {
    return std::any_of(terms.begin(), terms.end(), [&](std::string_view term) {
      return text::contains(text_value, term);
    });
  }
};

struct demo_environment {
  std::string api_key;
  std::string base_url;

  static demo_environment load() {
    demo_environment environment;
    environment.api_key = env_reader::get("OPENROUTER_API_KEY", env_reader::get("OPENAI_API_KEY"));
    environment.base_url =
      env_reader::get(
        "OPENAI_BASE_URL",
        env_reader::get("OPENROUTER_BASE_URL", "https://openrouter.ai/api"));
    return environment;
  }
};

class chat_model_plan {
public:
  explicit chat_model_plan(std::string_view base_url) {
    append(env_reader::get("OPENAI_CHAT_MODEL"));
    append(env_reader::get("OPENROUTER_CHAT_MODEL"));
    append(uses_openrouter(base_url) ? "inclusionai/ring-2.6-1t:free" : "gpt-4o-mini");

    if (uses_openrouter(base_url)) {
      append("openrouter/owl-alpha");
      append("google/gemini-3.1-flash-lite");
      append("~google/gemini-flash-latest");
      append("openrouter/auto");
    }
    else {
      append("gpt-4.1-mini");
      append("gpt-4o-mini");
    }
  }

  const std::vector<std::string>& candidates() const noexcept {
    return candidates_;
  }

  const std::string& first() const {
    return candidates_.front();
  }

private:
  static bool uses_openrouter(std::string_view base_url) {
    return text::contains(base_url, "openrouter.ai");
  }

  void append(std::string value) {
    if (!value.empty() &&
        std::find(candidates_.begin(), candidates_.end(), value) == candidates_.end()) {
      candidates_.push_back(std::move(value));
    }
  }

  std::vector<std::string> candidates_;
};

class guideline_keyword_embedding_model final : public memory::embedding_model {
public:
  std::vector<float> embed(std::string_view input) const override {
    const auto value = text::lower(input);
    std::vector<float> embedding;
    embedding.reserve(std::size(keywords_) + 1);
    for (const auto keyword : keywords_) {
      embedding.push_back(count(value, keyword));
    }
    embedding.push_back(1.0F);
    normalize(embedding);
    return embedding;
  }

private:
  static float count(const std::string& value, std::string_view needle) {
    float result = 0.0F;
    std::size_t offset = 0;
    while (offset < value.size()) {
      const auto found = value.find(needle, offset);
      if (found == std::string::npos) {
        break;
      }
      result += 1.0F;
      offset = found + needle.size();
    }
    return result;
  }

  static void normalize(std::vector<float>& embedding) {
    float norm = 0.0F;
    for (const auto value : embedding) {
      norm += value * value;
    }
    norm = std::sqrt(norm);
    if (norm <= 0.0F) {
      return;
    }
    for (auto& value : embedding) {
      value /= norm;
    }
  }

  static constexpr std::string_view keywords_[] {
    "new", "delete", "allocation", "dynamic", "resource", "handle", "manager", "object",
    "smart", "unique_ptr", "make_unique", "vector", "raii", "r.11", "r.12",
  };
};

class guideline_rule_reranker final : public knowledge::knowledge_reranker {
public:
  std::vector<knowledge::knowledge_result> rerank(
    const knowledge::knowledge_query& query,
    std::vector<knowledge::knowledge_result> candidates) const override {
    for (auto& candidate : candidates) {
      candidate.score = score(query, candidate);
    }

    std::sort(candidates.begin(), candidates.end(), [](const auto& lhs, const auto& rhs) {
      if (lhs.score != rhs.score) {
        return lhs.score > rhs.score;
      }
      if (lhs.chunk.start_offset != rhs.chunk.start_offset) {
        return lhs.chunk.start_offset < rhs.chunk.start_offset;
      }
      return lhs.chunk.id < rhs.chunk.id;
    });

    if (query.limit != 0 && candidates.size() > query.limit) {
      candidates.resize(query.limit);
    }
    return candidates;
  }

private:
  static std::string section_of(const knowledge::knowledge_chunk& chunk) {
    const auto section = chunk.metadata.find("section");
    return section == chunk.metadata.end() ? std::string {} : section->second;
  }

  static double score(
    const knowledge::knowledge_query& query,
    const knowledge::knowledge_result& candidate) {
    const auto section = text::lower(section_of(candidate.chunk));
    const auto content = text::lower(candidate.chunk.content);
    const auto query_text = text::lower(query.text);

    double result = candidate.score + rule_heading_bonus(section);
    const bool asks_new_delete =
      text::contains(query_text, "new") || text::contains(query_text, "delete");

    if (asks_new_delete && text::contains(content, "avoid calling new and delete explicitly")) {
      result += 7.0;
    }
    if (text::contains(content, "immediately give the result of an explicit resource allocation")) {
      result += 5.0;
    }
    if (text::contains(content, "manager object")) {
      result += 3.0;
    }
    if (text::contains(content, "make_unique") || text::contains(content, "unique_ptr") ||
        text::contains(content, "std::vector")) {
      result += 0.75;
    }
    if (section == "note") {
      result -= 2.0;
    }
    if (text::contains(content, "does not fall under this guideline") ||
        text::contains(content, "not being used to encode type information")) {
      result -= 6.0;
    }
    return result;
  }

  static double rule_heading_bonus(std::string_view section) {
    double result = 0.0;
    if (text::starts_with(section, "r.11:")) {
      result += 10.0;
    }
    if (text::starts_with(section, "r.12:")) {
      result += 9.0;
    }
    if (text::contains(section, "new") && text::contains(section, "delete")) {
      result += 5.0;
    }
    if (text::contains(section, "resource allocation")) {
      result += 5.0;
    }
    return result;
  }
};

struct retrieval_snapshot {
  knowledge::knowledge_retrieval_report report;
  std::string context_block;
};

struct answer_attempt {
  std::optional<knowledge::knowledge_answer_report> report;
  std::string model;
  std::vector<std::string> errors;
};

class url_rag_demo {
public:
  explicit url_rag_demo(demo_environment environment)
      : environment_(std::move(environment)),
        model_plan_(environment_.base_url),
        llm_client_(make_llm_client()),
        retriever_(make_retriever()),
        service_(retriever_, knowledge::knowledge_document_loader::make_default(), llm_client_) {
  }

  int run() {
    if (environment_.api_key.empty()) {
      wuwe::println(
        "[FAIL] url_rag_example requires OPENROUTER_API_KEY or OPENAI_API_KEY for the LLM call.");
      return 1;
    }

    print_header();
    const auto upload = upload_document();
    if (!upload.ingest.errors.empty()) {
      print_ingest_errors(upload.ingest.errors);
      return 1;
    }

    auto retrieval = retrieve_and_validate();
    print_retrieval(upload, retrieval);

    auto answer = ask_llm();
    if (!answer.report.has_value()) {
      print_llm_errors(answer.errors);
      return 1;
    }

    print_answer(answer);
    return 0;
  }

private:
  std::shared_ptr<wuwe::openai_compatible_llm_client> make_llm_client() const {
    return std::make_shared<wuwe::openai_compatible_llm_client>(wuwe::llm_client_config {
      .base_url = environment_.base_url,
      .api_key = environment_.api_key,
      .model = model_plan_.first(),
      .timeout = 120000,
    });
  }

  static std::shared_ptr<knowledge::knowledge_retriever> make_retriever() {
    auto retriever = std::make_shared<knowledge::knowledge_retriever>(
      std::make_shared<knowledge::in_memory_knowledge_store>(),
      std::make_shared<knowledge::in_memory_knowledge_index>(),
      std::make_shared<guideline_keyword_embedding_model>(),
      knowledge::knowledge_splitter({
        .max_chars = 900,
        .overlap_chars = 100,
        .prefer_paragraph_boundaries = true,
        .include_document_summary_chunk = true,
        .document_summary_chars = 1800,
      }));
    retriever->set_reranker(std::make_shared<guideline_rule_reranker>());
    return retriever;
  }

  knowledge::knowledge_upload_report upload_document() {
    return service_.upload_document(
      std::string(kGuidelinesUrl),
      {
        .metadata = { { "collection", "cpp-core-guidelines" } },
      });
  }

  retrieval_snapshot retrieve_and_validate() const {
    knowledge::knowledge_query query;
    query.text = guideline_demo_spec::retrieval_query();
    query.limit = 4;
    query.candidate_limit = 80;
    query.vector_weight = 1.0;
    query.lexical_weight = 1.0;

    retrieval_snapshot snapshot;
    snapshot.report = retriever_->retrieve_detailed(query);
    if (snapshot.report.results.empty()) {
      throw std::runtime_error("no knowledge chunks were retrieved for the URL question");
    }

    knowledge::knowledge_context context(retriever_, context_policy());
    snapshot.context_block = context.build_context_block(guideline_demo_spec::retrieval_query());
    if (!guideline_demo_spec::context_is_relevant(snapshot.context_block)) {
      wuwe::println("[FAIL] retrieval quality gate did not find the expected rule evidence.");
      wuwe::println("Compact cited context preview:\n{}\n", text::preview(snapshot.context_block));
      throw std::runtime_error("retrieval quality gate failed");
    }
    return snapshot;
  }

  answer_attempt ask_llm() {
    answer_attempt attempt;
    for (const auto& model : model_plan_.candidates()) {
      auto candidate = service_.ask({
        .query = guideline_demo_spec::retrieval_query(),
        .model = model,
        .policy = context_policy(),
        .generate_answer = true,
      });
      if (candidate.answer) {
        if (guideline_demo_spec::answer_is_relevant(candidate.answer.content) &&
            citation_guard::answer_uses_context_citations(
              candidate.answer.content,
              candidate.context_block)) {
          attempt.model = model;
          attempt.report = std::move(candidate);
          return attempt;
        }
        attempt.errors.push_back(
          model + ": answer did not pass quality gate | " +
          text::preview(candidate.answer.content, 240));
        continue;
      }

      auto error = model + ": " + candidate.answer.error_code.message();
      if (!candidate.answer.content.empty()) {
        error += " | " + candidate.answer.content;
      }
      attempt.errors.push_back(std::move(error));
    }
    return attempt;
  }

  static knowledge::knowledge_policy context_policy() {
    knowledge::knowledge_policy policy;
    policy.max_results = 3;
    policy.candidate_results = 80;
    policy.max_context_chars = 3600;
    policy.surrounding_chunks_after = 1;
    policy.result_processing.merge_adjacent_chunks = false;
    policy.result_processing.max_merged_chars = 1000;
    return policy;
  }

  void print_header() const {
    wuwe::println("URL RAG example");
    wuwe::println("Loading external knowledge from:\n{}\n", kGuidelinesUrl);
    wuwe::println("LLM model candidates:");
    for (const auto& model : model_plan_.candidates()) {
      wuwe::println("- {}", model);
    }
    wuwe::println("");
  }

  static void print_ingest_errors(const std::vector<std::string>& errors) {
    wuwe::println("[FAIL] ingest errors:");
    for (const auto& error : errors) {
      wuwe::println("- {}", error);
    }
  }

  void print_llm_errors(const std::vector<std::string>& errors) const {
    wuwe::println("[FAIL] all LLM model candidates failed.");
    wuwe::println("Retrieval already passed; this failure is from the upstream LLM call.");
    wuwe::println("base_url={}", environment_.base_url);
    for (const auto& error : errors) {
      wuwe::println("- {}", error);
    }
  }

  static void print_retrieval(
    const knowledge::knowledge_upload_report& upload,
    const retrieval_snapshot& retrieval) {
    const auto& results = retrieval.report.results;
    wuwe::println("Question:\n{}\n", kQuestion);
    wuwe::println("Loaded documents={} ingested={} skipped={} retrieved={}",
      upload.documents,
      upload.ingest.ingested,
      upload.ingest.skipped,
      results.size());
    wuwe::println("source_uri={}\n", results.front().chunk.source_uri);
    wuwe::println("Top evidence sections after rerank:");
    for (std::size_t index = 0; index < results.size(); ++index) {
      const auto section = results[index].chunk.metadata.find("section");
      wuwe::println("{}. score={} section={}",
        index + 1,
        results[index].score,
        section == results[index].chunk.metadata.end() ? "(none)" : section->second);
    }
    wuwe::println("");
    wuwe::println("Cited context sent to the LLM:\n{}\n", text::preview(retrieval.context_block, 5000));
    wuwe::println("[PASS] Retrieval quality gate found R.11, R.12, and resource-manager evidence.\n");
  }

  void print_answer(const answer_attempt& answer) const {
    const auto tool_result = knowledge::knowledge_tool_provider(*retriever_, {
      .max_search_results = 3,
    }).invoke(
      "search_knowledge",
      std::string("{\"content\":") + nlohmann::json(guideline_demo_spec::retrieval_query()).dump() +
        ",\"limit\":3}");

    wuwe::println("answered_model={}\n", answer.model);
    wuwe::println("Generated LLM answer:\n{}\n", answer.report->answer.content);
    wuwe::println("LLM usage: prompt_tokens={} completion_tokens={} total_tokens={}\n",
      answer.report->answer.usage.prompt_tokens,
      answer.report->answer.usage.completion_tokens,
      answer.report->answer.usage.total_tokens);
    wuwe::println("Tool search result preview:\n{}\n", text::preview(tool_result.content, 1200));
    wuwe::println("[PASS] URL RAG retrieved external knowledge and the LLM answered from it.");
  }

  demo_environment environment_;
  chat_model_plan model_plan_;
  std::shared_ptr<wuwe::openai_compatible_llm_client> llm_client_;
  std::shared_ptr<knowledge::knowledge_retriever> retriever_;
  knowledge::knowledge_rag_service service_;
};

} // namespace

int main() {
  try {
    wuwe_example::configure_utf8_console();
    return url_rag_demo(demo_environment::load()).run();
  }
  catch (const std::exception& ex) {
    wuwe::println("[FAIL] {}", ex.what());
    return 1;
  }
}
