#include <exception>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <wuwe/agent/knowledge/in_memory_knowledge_index.hpp>
#include <wuwe/agent/knowledge/in_memory_knowledge_store.hpp>
#include <wuwe/agent/knowledge/code_knowledge_loader.hpp>
#include <wuwe/agent/knowledge/directory_knowledge_loader.hpp>
#include <wuwe/agent/knowledge/file_knowledge_index.hpp>
#include <wuwe/agent/knowledge/file_knowledge_loader.hpp>
#include <wuwe/agent/knowledge/file_knowledge_store.hpp>
#include <wuwe/agent/knowledge/knowledge_benchmark.hpp>
#include <wuwe/agent/knowledge/knowledge_cache.hpp>
#include <wuwe/agent/knowledge/knowledge_context.hpp>
#include <wuwe/agent/knowledge/knowledge_document_loader.hpp>
#include <wuwe/agent/knowledge/knowledge_eval.hpp>
#include <wuwe/agent/knowledge/knowledge_grounding.hpp>
#include <wuwe/agent/knowledge/knowledge_metrics.hpp>
#include <wuwe/agent/knowledge/knowledge_migration.hpp>
#include <wuwe/agent/knowledge/knowledge_observability.hpp>
#include <wuwe/agent/knowledge/knowledge_parser_registry.hpp>
#include <wuwe/agent/knowledge/knowledge_path.hpp>
#include <wuwe/agent/knowledge/knowledge_pipeline.hpp>
#include <wuwe/agent/knowledge/knowledge_pipeline_config.hpp>
#include <wuwe/agent/knowledge/knowledge_query_rewriter.hpp>
#include <wuwe/agent/knowledge/knowledge_rag_service.hpp>
#include <wuwe/agent/knowledge/knowledge_result_processor.hpp>
#include <wuwe/agent/knowledge/knowledge_tools.hpp>
#include <wuwe/agent/knowledge/qdrant_knowledge_index.hpp>
#include <wuwe/agent/knowledge/remote_vector_knowledge_index.hpp>
#include <wuwe/agent/knowledge/sqlite_knowledge_index.hpp>
#include <wuwe/agent/knowledge/structured_knowledge_loader.hpp>
#include <wuwe/agent/knowledge/tika_knowledge_loader.hpp>
#include <wuwe/agent/memory/embedding_model.hpp>
#include <wuwe/agent/llm/llm_types.h>
#include <wuwe/common/print.h>
#include <wuwe/net/http_client.h>

#include <nlohmann/json.hpp>

namespace {

using namespace wuwe;
using namespace wuwe::agent::knowledge;

void require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

bool contains(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

std::string env_value(const char* name) {
#if defined(_MSC_VER)
  char* value {};
  std::size_t size {};
  if (_dupenv_s(&value, &size, name) != 0 || !value) {
    return {};
  }
  std::string result(value);
  std::free(value);
  return result;
#else
  const auto* value = std::getenv(name);
  if (!value) {
    return {};
  }
  return value;
#endif
}

class topic_embedding_model final : public agent::memory::embedding_model {
public:
  std::vector<float> embed(std::string_view text) const override {
    const std::string value(text);
    if (contains(value, "API") || contains(value, "api")) {
      return { 1.0F, 0.0F, 0.0F };
    }
    if (contains(value, "retrieval") || contains(value, "Retrieval") ||
        contains(value, "RAG")) {
      return { 0.0F, 1.0F, 0.0F };
    }
    return { 0.0F, 0.0F, 1.0F };
  }
};

class fake_llm_client final : public llm_client {
public:
  explicit fake_llm_client(std::string content) : content_(std::move(content)) {
  }

  llm_response complete(const llm_request&) override {
    return {
      .content = content_,
    };
  }

private:
  std::string content_;
};

class selectively_failing_embedding_model final : public agent::memory::embedding_model {
public:
  std::vector<float> embed(std::string_view text) const override {
    if (contains(std::string(text), "broken")) {
      throw std::runtime_error("embedding failed");
    }
    return { 1.0F, 0.0F, 0.0F };
  }
};

class slow_embedding_model final : public agent::memory::embedding_model {
public:
  std::vector<float> embed(std::string_view) const override {
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    return { 1.0F, 0.0F, 0.0F };
  }
};

class flaky_embedding_model final : public agent::memory::embedding_model {
public:
  std::vector<float> embed(std::string_view text) const override {
    const auto value = std::string(text);
    auto& count = attempts[value];
    ++count;
    if (count == 1) {
      throw std::runtime_error("transient embedding failure");
    }
    return { 1.0F, 0.0F, 0.0F };
  }

  mutable std::map<std::string, int> attempts;
};

class qdrant_knowledge_capture_http_client final : public http_client {
public:
  http_response send(const http_request& request) override {
    requests.push_back(request);
    if (request.method == "POST" && contains(request.url, "/points/search")) {
      return { .body = search_response };
    }
    return { .body = R"({"result":{"status":"ok"}})" };
  }

  std::string search_response { R"({"result":[]})" };
  std::vector<http_request> requests;
};

class tika_knowledge_capture_http_client final : public http_client {
public:
  http_response send(const http_request& request) override {
    requests.push_back(request);
    for (const auto& [key, value] : request.headers) {
      if (key == "Accept" && value == "text/html" && !html_response_body.empty()) {
        return { .body = html_response_body };
      }
    }
    return { .body = response_body };
  }

  std::string response_body { "Parsed document text from Tika." };
  std::string html_response_body;
  std::vector<http_request> requests;
};

class remote_vector_capture_http_client final : public http_client {
public:
  http_response send(const http_request& request) override {
    requests.push_back(request);
    return { .body = response_body };
  }

  std::string response_body { R"({"results":[]})" };
  std::vector<http_request> requests;
};

class reranker_capture_http_client final : public http_client {
public:
  http_response send(const http_request& request) override {
    requests.push_back(request);
    return { .body = response_body };
  }

  std::string response_body { R"({"score":4.2})" };
  std::vector<http_request> requests;
};

class keyword_cross_encoder_scorer final : public cross_encoder_knowledge_scorer {
public:
  double score(const knowledge_query&, const knowledge_result& candidate) const override {
    return contains(candidate.chunk.content, "preferred") ? 2.0 : 0.1;
  }
};

class counting_knowledge_reranker final : public knowledge_reranker {
public:
  std::vector<knowledge_result> rerank(
    const knowledge_query& query,
    std::vector<knowledge_result> candidates) const override {
    ++calls;
    if (query.limit != 0 && candidates.size() > query.limit) {
      candidates.resize(query.limit);
    }
    return candidates;
  }

  mutable int calls {};
};

std::shared_ptr<knowledge_retriever> make_retriever(chunking_policy policy = {}) {
  return std::make_shared<knowledge_retriever>(
    std::make_shared<in_memory_knowledge_store>(),
    std::make_shared<in_memory_knowledge_index>(),
    std::make_shared<topic_embedding_model>(),
    knowledge_splitter(policy));
}

std::filesystem::path unique_temp_path(std::string_view suffix) {
  return std::filesystem::temp_directory_path() /
         ("wuwe-knowledge-test-" +
          std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
          std::string(suffix));
}

std::filesystem::path fixture_path(std::string_view relative) {
  const auto path = std::filesystem::current_path() / "tests" / "fixtures" /
                    std::filesystem::path(relative);
  if (std::filesystem::exists(path)) {
    return path;
  }
  return std::filesystem::current_path() / "fixtures" / std::filesystem::path(relative);
}

void test_splitter_chunks_with_overlap() {
  knowledge_splitter splitter({
    .max_chars = 10,
    .overlap_chars = 2,
  });

  const auto chunks = splitter.split({
    .id = "doc",
    .content = "abcdefghijklmnopqrstuvwxyz",
  });

  require(chunks.size() == 3, "splitter should create overlapping chunks");
  require(chunks[0].content == "abcdefghij", "first chunk should start at zero");
  require(chunks[1].content == "ijklmnopqr", "second chunk should include overlap");
  require(chunks[2].content == "qrstuvwxyz", "third chunk should include final overlap");
}

void test_splitter_respects_markdown_headings() {
  knowledge_splitter splitter({
    .max_chars = 200,
    .overlap_chars = 0,
    .respect_markdown_headings = true,
  });

  const auto chunks = splitter.split({
    .id = "doc",
    .title = "Guide",
    .content = "# Overview\nGeneral introduction.\n## Retrieval\nRAG searches documents.\n",
  });

  require(chunks.size() == 2, "markdown splitter should create one chunk per section");
  require(chunks[0].metadata.at("section") == "Overview",
    "first markdown chunk should record section title");
  require(chunks[1].metadata.at("section") == "Retrieval",
    "second markdown chunk should record nested section title");
  require(chunks[1].start_line == 3, "markdown chunk should record start line");
  require(contains(chunks[1].content, "RAG searches documents."),
    "section chunk should contain section body");
}

void test_splitter_skips_markdown_headings_for_tika_documents() {
  knowledge_splitter splitter({
    .max_chars = 200,
    .overlap_chars = 0,
    .respect_markdown_headings = true,
  });

  const auto chunks = splitter.split({
    .id = "pdf",
    .content = "# Not a real markdown heading\nTika extracted PDF text.",
    .metadata = {
      { "extension", ".pdf" },
      { "parser", "tika" },
    },
  });

  require(chunks.size() == 1, "tika documents should use normal chunking");
  require(!chunks.front().metadata.contains("section"),
    "tika documents should not infer markdown sections");
}

void test_splitter_adds_pdf_page_and_section_metadata() {
  knowledge_splitter splitter({
    .max_chars = 45,
    .overlap_chars = 0,
  });

  const auto chunks = splitter.split({
    .id = "pdf",
    .content = "Chapter 1 Foundations\nfirst page content keeps going.\f"
               "second page has more agent text for retrieval.",
    .metadata = {
      { "parser", "tika" },
      { "extension", ".pdf" },
    },
  });

  require(chunks.size() >= 2, "pdf text should be split into chunks");
  require(chunks.front().metadata.at("section") == "Chapter 1 Foundations",
    "pdf chunks should infer conservative section titles");
  require(chunks.front().metadata.at("page_start") == "1",
    "pdf chunks should record start page");
  require(chunks.back().metadata.at("page_start") == "2",
    "pdf chunks should record page after form-feed break");
}

void test_splitter_avoids_pdf_bibliography_items_as_sections() {
  knowledge_splitter splitter({
    .max_chars = 80,
    .overlap_chars = 0,
  });

  const auto chunks = splitter.split({
    .id = "pdf",
    .content = "3.\t Market.us. Global Agentic AI Market Size, Trends and Forecast 2025-2034.\n"
               "This bibliography item should not become a section title.",
    .metadata = {
      { "parser", "tika" },
      { "extension", ".pdf" },
    },
  });

  require(!chunks.empty(), "pdf bibliography fixture should produce chunks");
  require(!chunks.front().metadata.contains("section"),
    "bibliography references should not be inferred as PDF sections");
}

void test_splitter_can_add_document_summary_chunk() {
  knowledge_splitter splitter({
    .max_chars = 60,
    .overlap_chars = 0,
    .include_document_summary_chunk = true,
  });

  const auto chunks = splitter.split({
    .id = "patterns",
    .title = "Agentic Patterns",
    .content = "Agentic systems use patterns.\n\n"
               "1. Prompt Chaining\n"
               "2. Tool Use\n"
               "3. Multi-Agent Collaboration\n"
               "The rest of the document explains implementation details.",
    .source_uri = "docs/patterns.md",
  });

  require(chunks.size() > 1, "summary chunk should be added before normal chunks");
  require(chunks.front().id == "patterns#summary",
    "summary chunk should use stable summary id");
  require(chunks.front().metadata.at("chunking") == "document_summary",
    "summary chunk should be marked in metadata");
  require(contains(chunks.front().content, "Prompt Chaining"),
    "summary chunk should collect likely section lines");
  require(contains(chunks.front().content, "Tool Use"),
    "summary chunk should help broad pattern queries");
}

void test_splitter_summary_chunk_uses_llm_summary_and_toc() {
  knowledge_splitter splitter({
    .max_chars = 80,
    .overlap_chars = 0,
    .include_document_summary_chunk = true,
    .document_summary_chars = 2000,
  });

  const auto chunks = splitter.split({
    .id = "patterns",
    .title = "Agentic Patterns",
    .content = "Part I\n"
               "Prompt Chaining Pattern Overview    3\n"
               "Tool Use Pattern Overview    61\n"
               "Knowledge Retrieval (RAG) Pattern Overview    193\n"
               "body text",
    .metadata = {
      { "summary", "This guide covers prompt chaining, tool use, and RAG." },
    },
  });

  require(contains(chunks.front().content, "LLM summary"),
    "summary chunk should include enriched LLM summary");
  require(contains(chunks.front().content, "Prompt Chaining Pattern"),
    "summary chunk should include extracted TOC pattern lines");
  require(contains(chunks.front().content, "Knowledge Retrieval (RAG) Pattern"),
    "summary chunk should preserve RAG pattern TOC entries");
  const auto toc_entries = nlohmann::json::parse(chunks.front().metadata.at("toc_entries"));
  require(toc_entries.is_array() && toc_entries.size() == 3,
    "summary chunk should expose structured TOC entries metadata");
  require(toc_entries[0] == "Prompt Chaining Pattern",
    "structured TOC entries should keep cleaned pattern names");
}

void test_path_to_utf8_preserves_unicode_on_windows() {
#if defined(_WIN32)
  const std::filesystem::path path(L"C:\\Users\\ligx\\Downloads\\Gull\u00ED.pdf");
  const auto value = filename_to_utf8(path);
  require(value.find("Gull") != std::string::npos, "utf8 path should keep ascii prefix");
  require(value.find('?') == std::string::npos, "utf8 path should not replace unicode with ?");
#endif
}

void test_splitter_prefers_paragraph_boundaries() {
  knowledge_splitter splitter({
    .max_chars = 34,
    .overlap_chars = 0,
    .respect_markdown_headings = false,
    .prefer_paragraph_boundaries = true,
  });

  const auto chunks = splitter.split({
    .id = "doc",
    .content = "First paragraph stays whole.\n\nSecond paragraph is separate.",
  });

  require(chunks.size() == 2, "paragraph-aware splitter should split at blank line");
  require(chunks[0].content == "First paragraph stays whole.\n\n",
    "paragraph-aware splitter should keep paragraph boundary");
  require(contains(chunks[1].content, "Second paragraph"),
    "paragraph-aware splitter should keep remaining paragraph");
}

void test_splitter_supports_token_windows() {
  knowledge_splitter splitter({
    .max_chars = 100,
    .overlap_chars = 0,
    .max_tokens = 4,
    .overlap_tokens = 1,
    .respect_markdown_headings = false,
  });

  const auto chunks = splitter.split({
    .id = "doc",
    .content = "one two three four five six seven",
  });

  require(chunks.size() == 2, "token-aware splitter should split by token count");
  require(chunks[0].content == "one two three four",
    "first token chunk should contain max token window");
  require(chunks[1].content == "four five six seven",
    "second token chunk should include token overlap");
  require(chunks[0].metadata.at("chunking") == "token",
    "token-aware splitter should mark chunking strategy");
}

void test_splitter_avoids_splitting_markdown_code_fences() {
  knowledge_splitter splitter({
    .max_chars = 48,
    .overlap_chars = 0,
    .respect_markdown_headings = false,
    .prefer_paragraph_boundaries = true,
    .protect_markdown_code_fences = true,
  });

  const auto chunks = splitter.split({
    .id = "doc",
    .content =
      "Intro paragraph before code.\n\n"
      "```cpp\n"
      "int main() {\n"
      "  return 0;\n"
      "}\n"
      "```\n"
      "Closing paragraph after code.",
  });

  require(chunks.size() >= 2, "code-fence-aware splitter should create multiple chunks");
  require(!contains(chunks[0].content, "```cpp"),
    "code-fence-aware splitter should avoid starting a fenced block in the previous chunk");
  require(contains(chunks[1].content, "```cpp") && contains(chunks[1].content, "```"),
    "code-fence-aware splitter should keep fenced code together");
}

void test_splitter_respects_code_symbols() {
  knowledge_splitter splitter({
    .max_chars = 200,
    .overlap_chars = 0,
    .respect_markdown_headings = false,
    .respect_code_symbols = true,
  });

  const auto chunks = splitter.split({
    .id = "code",
    .content =
      "#include <iostream>\n"
      "class Runner {\n"
      "public:\n"
      "  void run() {}\n"
      "};\n"
      "int main() {\n"
      "  Runner{}.run();\n"
      "}\n",
    .metadata = { { "extension", ".cpp" } },
  });

  require(chunks.size() >= 2, "code-aware splitter should split by symbols");
  require(chunks[0].metadata.at("chunking") == "code_symbol",
    "code-aware splitter should mark symbol chunks");
  require(std::any_of(chunks.begin(), chunks.end(), [](const knowledge_chunk& chunk) {
      return contains(chunk.content, "int main");
    }),
    "code-aware splitter should create a chunk for function symbols");
}

void test_file_loader_loads_document() {
  const auto path = unique_temp_path(".md");

  {
    std::ofstream output(path, std::ios::binary);
    output << "# Retrieval\nRAG loads external documents.\n";
  }

  const auto cleanup = [&] {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
  };

  file_knowledge_loader loader;
  const auto document = loader.load(path, {
    .metadata = { { "topic", "rag" } },
  });

  require(document.id == path.filename().string(), "file loader should derive id from filename");
  require(document.title == path.stem().string(), "file loader should derive title from stem");
  require(document.source_uri == path.generic_string(),
    "file loader should use path as default source uri");
  require(document.metadata.at("topic") == "rag", "file loader should preserve metadata");
  require(document.metadata.at("extension") == ".md", "file loader should record extension");
  require(contains(document.content, "RAG loads external documents."),
    "file loader should read content");

  cleanup();
}

void test_file_loader_extracts_html_text() {
  const auto path = unique_temp_path(".html");

  {
    std::ofstream output(path, std::ios::binary);
    output << "<html><head><style>.hidden{}</style><script>alert(1)</script></head>"
              "<body><h1>Retrieval &amp; Citations</h1><p>RAG uses sources.</p></body></html>";
  }

  const auto cleanup = [&] {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
  };

  file_knowledge_loader loader;
  const auto document = loader.load(path);

  require(contains(document.content, "Retrieval & Citations"),
    "html loader should decode text entities");
  require(contains(document.content, "RAG uses sources."),
    "html loader should keep body text");
  require(!contains(document.content, "alert"),
    "html loader should remove script content");
  require(document.metadata.at("extension") == ".html",
    "html loader should record normalized extension");
  require(document.metadata.at("extracted_as") == "text",
    "html loader should record text extraction");

  cleanup();
}

void test_file_loader_extracts_rtf_text() {
  const auto path = unique_temp_path(".rtf");

  {
    std::ofstream output(path, std::ios::binary);
    output << R"({\rtf1\ansi{\fonttbl{\f0 Arial;}}\b Retrieval\b0\par RAG uses rich text sources.\line Citation\tab metadata.})";
  }

  const auto cleanup = [&] {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
  };

  file_knowledge_loader loader;
  const auto document = loader.load(path);

  require(contains(document.content, "Retrieval"),
    "rtf loader should keep visible text");
  require(contains(document.content, "RAG uses rich text sources."),
    "rtf loader should keep paragraph text");
  require(contains(document.content, "Citation metadata."),
    "rtf loader should convert tabs to spaces");
  require(!contains(document.content, "fonttbl"),
    "rtf loader should skip font table destinations");
  require(!contains(document.content, "\\b"),
    "rtf loader should remove formatting controls");
  require(document.metadata.at("extension") == ".rtf",
    "rtf loader should record normalized extension");
  require(document.metadata.at("content_type") == "application/rtf",
    "rtf loader should record rtf content type");
  require(document.metadata.at("extracted_as") == "text",
    "rtf loader should record text extraction");

  cleanup();
}

void test_tika_loader_extracts_remote_parser_text() {
  const auto path = unique_temp_path(".pdf");

  {
    std::ofstream output(path, std::ios::binary);
    output << "%PDF fake binary content";
  }

  const auto cleanup = [&] {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
  };

  auto http = std::make_shared<tika_knowledge_capture_http_client>();
  tika_knowledge_loader loader({
    .base_url = "http://tika.local/",
    .timeout_ms = 12000,
    .extract_pdf_pages = false,
  }, http);

  const auto document = loader.load(path, {
    .metadata = { { "topic", "rag" } },
  });

  require(document.content == "Parsed document text from Tika.",
    "tika loader should use parser response as document content");
  require(document.metadata.at("parser") == "tika",
    "tika loader should record parser metadata");
  require(document.metadata.at("content_type") == "application/pdf",
    "tika loader should infer PDF content type");
  require(document.metadata.at("extension") == ".pdf",
    "tika loader should record extension");
  require(document.metadata.at("topic") == "rag",
    "tika loader should preserve metadata");
  require(http->requests.size() == 1, "tika loader should send one HTTP request");
  require(http->requests.front().method == "PUT",
    "tika loader should send raw file content using PUT");
  require(http->requests.front().url == "http://tika.local/tika",
    "tika loader should normalize base URL and endpoint");
  require(http->requests.front().body == "%PDF fake binary content",
    "tika loader should send file bytes");

  bool has_accept_text = false;
  bool has_pdf_content_type = false;
  for (const auto& [key, value] : http->requests.front().headers) {
    if (key == "Accept" && value == "text/plain") {
      has_accept_text = true;
    }
    if (key == "Content-Type" && value == "application/pdf") {
      has_pdf_content_type = true;
    }
  }
  require(has_accept_text, "tika loader should request plain text");
  require(has_pdf_content_type, "tika loader should send content type");

  cleanup();
}

void test_tika_loader_extracts_pdf_page_breaks_from_html() {
  const auto path = unique_temp_path(".pdf");

  {
    std::ofstream output(path, std::ios::binary);
    output << "%PDF fake binary content";
  }

  const auto cleanup = [&] {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
  };

  auto http = std::make_shared<tika_knowledge_capture_http_client>();
  http->response_body = "plain fallback";
  http->html_response_body =
    R"(<html><body><div class="page"><p>Page 1 text</p></div><div class="page"><p>Page 2 text</p></div></body></html>)";

  tika_knowledge_loader loader({
    .base_url = "http://tika.local/",
  }, http);

  const auto document = loader.load(path);
  require(contains(document.content, "Page 1 text") &&
          contains(document.content, "Page 2 text") &&
          contains(document.content, "\f"),
    "tika loader should join HTML page divs with form-feed");
  require(document.metadata.at("page_count") == "2",
    "tika loader should record page count from HTML extraction");
  require(document.metadata.at("page_extraction") == "tika-html",
    "tika loader should record page extraction source");
  require(http->requests.size() == 2,
    "tika PDF loader should request text and page HTML");

  cleanup();
}

void test_tika_loader_live_integration_when_configured() {
  const auto tika_url = env_value("WUWE_TIKA_URL");
  if (tika_url.empty()) {
    println("[SKIP] tika loader live integration requires WUWE_TIKA_URL");
    return;
  }

  const auto path = unique_temp_path(".txt");
  {
    std::ofstream output(path, std::ios::binary);
    output << "Tika live parser text.";
  }

  const auto cleanup = [&] {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
  };

  tika_knowledge_loader loader({
    .base_url = tika_url,
  });
  const auto document = loader.load(path, {
    .content_type = "text/plain",
  });
  require(contains(document.content, "Tika live parser text"),
    "tika live integration should return parsed text");

  cleanup();
}

void test_parser_registry_selects_file_and_tika_parsers() {
  const auto txt_path = unique_temp_path(".txt");
  const auto pdf_path = unique_temp_path(".pdf");
  {
    std::ofstream(txt_path, std::ios::binary) << "Registry file parser text.";
    std::ofstream(pdf_path, std::ios::binary) << "%PDF registry parser";
  }

  const auto cleanup = [&] {
    std::error_code ignored;
    std::filesystem::remove(txt_path, ignored);
    std::filesystem::remove(pdf_path, ignored);
  };

  auto http = std::make_shared<tika_knowledge_capture_http_client>();
  http->response_body = "Registry Tika parser text.";

  knowledge_parser_registry registry;
  registry.register_parser(std::make_shared<file_knowledge_document_parser>());
  registry.register_parser(std::make_shared<tika_knowledge_document_parser>(
    tika_knowledge_loader({
      .base_url = "http://tika.local",
      .extract_pdf_pages = false,
    }, http)));

  const auto text_document = registry.parse(txt_path);
  const auto pdf_document = registry.parse(pdf_path);
  require(contains(text_document.content, "Registry file parser text"),
    "parser registry should use file parser for text files");
  require(pdf_document.content == "Registry Tika parser text.",
    "parser registry should use Tika parser for PDF files");
  require(http->requests.size() == 1,
    "parser registry should call Tika only for Tika-supported files");

  cleanup();
}

void test_document_loader_loads_files_with_stable_metadata() {
  const auto root = unique_temp_path("");
  std::filesystem::create_directories(root);
  const auto path = root / "Agent Guide.md";
  {
    std::ofstream(path, std::ios::binary) << "# Agent Guide\nRAG retrieval notes.";
  }

  const auto cleanup = [&] {
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  };

  auto loader = knowledge_document_loader::make_default();
  const auto documents = loader.load(root, {
    .metadata = { { "collection", "docs" } },
  });

  require(documents.size() == 1, "document loader should parse supported files");
  require(documents.front().id == "Agent-Guide.md",
    "document loader should derive stable sanitized ids");
  require(documents.front().source_uri == "Agent Guide.md",
    "document loader should keep readable source uri");
  require(documents.front().metadata.at("relative_path") == "Agent Guide.md",
    "document loader should record relative path metadata");
  require(documents.front().metadata.at("collection") == "docs",
    "document loader should apply caller metadata");

  cleanup();
}

void test_document_loader_runs_enrichers() {
  const auto path = unique_temp_path(".md");
  {
    std::ofstream output(path, std::ios::binary);
    output << "# Agentic Patterns\nPrompt Chaining Pattern Overview";
  }

  const auto cleanup = [&] {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
  };

  auto enricher = std::make_shared<llm_knowledge_document_enricher>(
    std::make_shared<fake_llm_client>("LLM summary mentions Prompt Chaining."),
    llm_knowledge_document_enricher_config {
      .model = "summary-model",
    });

  const auto documents = knowledge_document_loader::make_default().load(path, {
    .enrichers = { enricher },
  });

  require(documents.size() == 1, "document loader should load fixture file");
  require(documents.front().metadata.at("summary") == "LLM summary mentions Prompt Chaining.",
    "document loader should apply LLM summary enricher");
  require(documents.front().metadata.at("summary_model") == "summary-model",
    "document loader should record summary model");

  cleanup();
}

void test_structured_loader_loads_csv_rows() {
  const auto path = unique_temp_path(".csv");
  {
    std::ofstream output(path, std::ios::binary);
    output << "name,role\nAda,admin\nBob,reader\n";
  }

  const auto cleanup = [&] {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
  };

  structured_knowledge_loader loader;
  const auto document = loader.load_csv(path);

  require(contains(document.content, "Row 1:"), "csv loader should emit row labels");
  require(contains(document.content, "name: Ada"), "csv loader should map headers to values");
  require(contains(document.content, "role: reader"), "csv loader should include later rows");
  require(document.metadata.at("structured_as") == "csv_rows",
    "csv loader should record structured format");
  require(document.metadata.at("row_count") == "2", "csv loader should record data row count");

  cleanup();
}

void test_structured_loader_flattens_json_paths() {
  const auto path = unique_temp_path(".json");
  {
    std::ofstream output(path, std::ios::binary);
    output << R"({"service":{"name":"search","retries":3},"features":["rag","memory"]})";
  }

  const auto cleanup = [&] {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
  };

  structured_knowledge_loader loader;
  const auto document = loader.load_json(path);

  require(contains(document.content, "$.service.name: search"),
    "json loader should flatten object paths");
  require(contains(document.content, "$.features[0]: rag"),
    "json loader should flatten array paths");
  require(document.metadata.at("structured_as") == "json_paths",
    "json loader should record structured format");

  cleanup();
}

void test_structured_loader_summarizes_openapi_json() {
  const auto path = unique_temp_path(".json");
  {
    std::ofstream output(path, std::ios::binary);
    output << R"({
      "openapi":"3.1.0",
      "info":{"title":"Search API","version":"1.0"},
      "paths":{
        "/knowledge/search":{
          "post":{
            "summary":"Search knowledge chunks",
            "operationId":"searchKnowledge",
            "tags":["knowledge"]
          }
        }
      }
    })";
  }

  const auto cleanup = [&] {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
  };

  structured_knowledge_loader loader;
  const auto document = loader.load_openapi_json(path);

  require(contains(document.content, "POST /knowledge/search") ||
          contains(document.content, "post /knowledge/search"),
    "openapi loader should summarize operations");
  require(contains(document.content, "Search knowledge chunks"),
    "openapi loader should include operation summary");
  require(document.metadata.at("structured_as") == "openapi_operations",
    "openapi loader should record structured format");

  cleanup();
}

void test_directory_loader_loads_supported_files() {
  const auto root = unique_temp_path("");
  std::filesystem::create_directories(root / "nested");
  {
    std::ofstream(root / "guide.md") << "# Guide\nRAG retrieval guide.\n";
    std::ofstream(root / "nested" / "api.txt") << "API reference.\n";
    std::ofstream(root / "ignore.bin") << "ignored";
  }

  const auto cleanup = [&] {
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  };

  directory_knowledge_loader loader;
  const auto documents = loader.load(root, {
    .metadata = { { "collection", "docs" } },
  });

  require(documents.size() == 2, "directory loader should load supported files");
  require(documents[0].metadata.at("collection") == "docs",
    "directory loader should preserve metadata");
  require(documents[0].metadata.contains("relative_path"),
    "directory loader should record relative path");
  require(documents[0].metadata.contains("content_hash"),
    "directory loader should record content hash");

  cleanup();
}

void test_code_loader_loads_repository_sources() {
  const auto root = unique_temp_path("");
  std::filesystem::create_directories(root / "src");
  std::filesystem::create_directories(root / "build");
  {
    std::ofstream(root / "src" / "main.cpp") << "int main() { return 0; }\n";
    std::ofstream(root / "src" / "tool.py") << "def run():\n  return 1\n";
    std::ofstream(root / "build" / "ignored.cpp") << "int ignored() { return 0; }\n";
    std::ofstream(root / "README.md") << "# ignored by code loader\n";
  }

  const auto cleanup = [&] {
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  };

  code_knowledge_loader loader;
  const auto documents = loader.load_repository(root);

  require(documents.size() == 2, "code loader should include source files and skip build output");
  require(documents[0].metadata.contains("relative_path"),
    "code loader should record relative path");
  require(documents[0].metadata.contains("language"),
    "code loader should record language");
  require(documents[0].metadata.at("language") == "cpp" ||
          documents[1].metadata.at("language") == "cpp",
    "code loader should infer cpp language");
  require(documents[0].metadata.at("extension") != ".md" &&
          documents[1].metadata.at("extension") != ".md",
    "code loader should ignore non-code files by default");

  cleanup();
}

void test_incremental_ingest_skips_updates_and_erases_stale() {
  const auto root = unique_temp_path("");
  std::filesystem::create_directories(root);
  const auto guide_path = root / "guide.md";
  const auto api_path = root / "api.md";
  std::ofstream(guide_path) << "# Retrieval\nRAG retrieval guide.\n";
  std::ofstream(api_path) << "# API\nAPI guide.\n";

  const auto cleanup = [&] {
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  };

  directory_knowledge_loader loader;
  auto retriever = make_retriever({
    .max_chars = 200,
    .overlap_chars = 0,
  });

  auto first = retriever->ingest_incremental(loader.load(root), true);
  require(first.ingested == 2, "first incremental ingest should ingest both documents");
  require(first.skipped == 0, "first incremental ingest should not skip documents");

  auto second = retriever->ingest_incremental(loader.load(root), true);
  require(second.ingested == 0, "unchanged documents should not be ingested again");
  require(second.skipped == 2, "unchanged documents should be skipped by content hash");

  std::ofstream(guide_path, std::ios::trunc) << "# Retrieval\nRAG retrieval updated guide.\n";
  std::filesystem::remove(api_path);

  auto third = retriever->ingest_incremental(loader.load(root), true);
  require(third.ingested == 1, "changed document should be reingested");
  require(third.erased_stale == 1, "missing document should be erased as stale");
  require(retriever->list_documents().size() == 1,
    "stale cleanup should leave one document");

  cleanup();
}

void test_file_store_reload_and_rebuild_index() {
  const auto path = unique_temp_path(".jsonl");
  const auto cleanup = [&] {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(path.string() + ".tmp", ignored);
  };
  cleanup();

  {
    auto retriever = std::make_shared<knowledge_retriever>(
      std::make_shared<file_knowledge_store>(path),
      std::make_shared<in_memory_knowledge_index>(),
      std::make_shared<topic_embedding_model>(),
      knowledge_splitter({
        .max_chars = 200,
        .overlap_chars = 0,
      }));

    retriever->ingest({
      .id = "rag-doc",
      .title = "Retrieval Guide",
      .content = "RAG retrieval persists external knowledge chunks.",
      .source_uri = "docs/rag.md",
      .metadata = { { "topic", "rag" } },
    });
  }

  {
    auto retriever = std::make_shared<knowledge_retriever>(
      std::make_shared<file_knowledge_store>(path),
      std::make_shared<in_memory_knowledge_index>(),
      std::make_shared<topic_embedding_model>());

    require(retriever->rebuild_index() == 1,
      "rebuild_index should restore persisted chunks into the index");

    knowledge_query query;
    query.text = "RAG retrieval";
    query.limit = 1;
    const auto results = retriever->retrieve(query);
    require(results.size() == 1, "rebuilt index should retrieve persisted chunks");
    require(results.front().chunk.document_id == "rag-doc",
      "rebuilt result should preserve document id");
  }

  cleanup();
}

void test_file_index_reload_without_rebuild() {
  const auto store_path = unique_temp_path(".store.jsonl");
  const auto index_path = unique_temp_path(".index.jsonl");
  const auto cleanup = [&] {
    std::error_code ignored;
    std::filesystem::remove(store_path, ignored);
    std::filesystem::remove(store_path.string() + ".tmp", ignored);
    std::filesystem::remove(index_path, ignored);
    std::filesystem::remove(index_path.string() + ".tmp", ignored);
  };
  cleanup();

  {
    auto retriever = std::make_shared<knowledge_retriever>(
      std::make_shared<file_knowledge_store>(store_path),
      std::make_shared<file_knowledge_index>(index_path),
      std::make_shared<topic_embedding_model>(),
      knowledge_splitter({
        .max_chars = 200,
        .overlap_chars = 0,
      }));

    retriever->ingest({
      .id = "rag-doc",
      .title = "Retrieval Guide",
      .content = "RAG retrieval persists embeddings in the index.",
      .source_uri = "docs/rag.md",
    });
  }

  {
    auto retriever = std::make_shared<knowledge_retriever>(
      std::make_shared<file_knowledge_store>(store_path),
      std::make_shared<file_knowledge_index>(index_path),
      std::make_shared<topic_embedding_model>());

    knowledge_query query;
    query.text = "RAG retrieval";
    query.limit = 1;
    const auto results = retriever->retrieve(query);
    require(results.size() == 1, "file index should retrieve after reload without rebuild");
    require(results.front().chunk.document_id == "rag-doc",
      "file index should preserve indexed chunk metadata");
  }

  cleanup();
}

void test_sqlite_index_reload_without_rebuild() {
  const auto store_path = unique_temp_path(".sqlite-store.jsonl");
  const auto index_path = unique_temp_path(".sqlite-index.db");
  const auto cleanup = [&] {
    std::error_code ignored;
    std::filesystem::remove(store_path, ignored);
    std::filesystem::remove(store_path.string() + ".tmp", ignored);
    std::filesystem::remove(index_path, ignored);
  };
  cleanup();

#if WUWE_HAS_SQLITE
  {
    auto retriever = std::make_shared<knowledge_retriever>(
      std::make_shared<file_knowledge_store>(store_path),
      std::make_shared<sqlite_knowledge_index>(index_path),
      std::make_shared<topic_embedding_model>(),
      knowledge_splitter({
        .max_chars = 200,
        .overlap_chars = 0,
      }));

    retriever->ingest({
      .id = "rag-doc",
      .title = "Retrieval Guide",
      .content = "RAG retrieval persists embeddings in SQLite.",
      .source_uri = "docs/rag.md",
    });
  }

  {
    auto retriever = std::make_shared<knowledge_retriever>(
      std::make_shared<file_knowledge_store>(store_path),
      std::make_shared<sqlite_knowledge_index>(index_path),
      std::make_shared<topic_embedding_model>());

    knowledge_query query;
    query.text = "RAG retrieval";
    query.limit = 1;
    const auto results = retriever->retrieve(query);
    require(results.size() == 1, "sqlite index should retrieve after reload without rebuild");
    require(results.front().chunk.document_id == "rag-doc",
      "sqlite index should preserve indexed chunk metadata");
  }
#else
  bool threw = false;
  try {
    sqlite_knowledge_index index(index_path);
  }
  catch (const std::exception&) {
    threw = true;
  }
  require(threw, "sqlite knowledge index should report unavailable when SQLite is disabled");
#endif

  cleanup();
}

void test_qdrant_knowledge_upsert_payload_includes_chunk_metadata() {
  auto http = std::make_shared<qdrant_knowledge_capture_http_client>();
  qdrant_knowledge_index index(
    {
      .base_url = "http://qdrant.local/",
      .collection_name = "knowledge",
      .embedding_provider = "openai-compatible",
      .embedding_model = "embedding-test",
      .embedding_version = "2026-05-05",
      .index_schema_version = "3",
      .create_collection_if_missing = false,
    },
    http);

  knowledge_chunk chunk {
    .id = "rag-doc#chunk-1",
    .document_id = "rag-doc",
    .title = "Retrieval Guide",
    .content = "RAG retrieval qdrant payload.",
    .start_offset = 0,
    .end_offset = 28,
    .start_line = 1,
    .end_line = 1,
    .source_uri = "docs/rag.md",
    .metadata = {
      { "topic", "rag" },
      { "tenant_id", "tenant-a" },
    },
  };

  index.upsert_batch({ chunk }, { { 1.0F, 0.0F, 0.0F } });

  require(http->requests.size() == 1, "qdrant knowledge upsert should send one request");
  const auto body = nlohmann::json::parse(http->requests.front().body);
  const auto& payload = body["points"][0]["payload"];
  require(payload["chunk_id"] == "rag-doc#chunk-1",
    "qdrant knowledge payload should include chunk id");
  require(payload["document_id"] == "rag-doc",
    "qdrant knowledge payload should include document id");
  require(payload["embedding_provider"] == "openai-compatible",
    "qdrant knowledge payload should include embedding provider");
  require(payload["embedding_dimension"] == 3,
    "qdrant knowledge payload should include embedding dimension");
  require(payload["index_schema_version"] == "3",
    "qdrant knowledge payload should include schema version");
  require(payload["metadata"]["topic"] == "rag",
    "qdrant knowledge payload should preserve metadata");
  require(payload["tenant_id"] == "tenant-a",
    "qdrant knowledge payload should lift tenant id for filtering");
}

void test_qdrant_knowledge_search_builds_filters_and_parses_results() {
  auto http = std::make_shared<qdrant_knowledge_capture_http_client>();
  qdrant_knowledge_index index(
    {
      .base_url = "http://qdrant.local/",
      .collection_name = "knowledge",
      .create_collection_if_missing = false,
    },
    http);

  knowledge_chunk chunk {
    .id = "rag-doc#chunk-1",
    .document_id = "rag-doc",
    .title = "Retrieval Guide",
    .content = "RAG retrieval qdrant search.",
    .source_uri = "docs/rag.md",
    .metadata = {
      { "topic", "rag" },
      { "tenant_id", "tenant-a" },
      { "allowed_roles", "reader" },
    },
  };

  http->search_response = nlohmann::json {
    { "result", nlohmann::json::array({
      {
        { "score", 0.9 },
        { "payload", {
          { "chunk", wuwe::agent::knowledge::detail::knowledge_chunk_to_json(chunk) },
        } },
      },
    }) },
  }.dump();

  knowledge_query query;
  query.text = "RAG retrieval";
  query.limit = 3;
  query.filters = { { "topic", "rag" } };
  query.access.tenant_id = "tenant-a";
  query.access.roles = { "reader" };

  const auto results = index.search(query, { 1.0F, 0.0F, 0.0F });
  require(results.size() == 1, "qdrant knowledge search should parse results");
  require(results.front().chunk.document_id == "rag-doc",
    "qdrant knowledge search should restore chunk payload");
  require(results.front().vector_score == 0.9,
    "qdrant knowledge search should expose vector score");

  require(http->requests.size() == 1, "qdrant knowledge search should send one request");
  const auto body = nlohmann::json::parse(http->requests.front().body);
  const auto& must = body["filter"]["must"];
  require(must.size() == 2,
    "qdrant knowledge search should push tenant and metadata filters");
  require(must[0]["key"] == "tenant_id",
    "qdrant knowledge search should push tenant filter");
  require(must[1]["key"] == "metadata.topic",
    "qdrant knowledge search should push metadata filter");
}

void test_qdrant_knowledge_live_integration_when_configured() {
  const auto url = env_value("WUWE_QDRANT_URL");
  if (url.empty()) {
    println("[SKIP] qdrant knowledge live integration requires WUWE_QDRANT_URL");
    return;
  }

  qdrant_knowledge_index index({
    .base_url = url,
    .collection_name = env_value("WUWE_QDRANT_KNOWLEDGE_COLLECTION").empty()
      ? "wuwe_knowledge_live_test"
      : env_value("WUWE_QDRANT_KNOWLEDGE_COLLECTION"),
    .embedding_provider = "test",
    .embedding_model = "deterministic",
    .embedding_version = "live-test",
    .index_schema_version = "1",
  });

  const auto suffix = std::to_string(std::chrono::steady_clock::now()
                                       .time_since_epoch()
                                       .count());
  knowledge_chunk retrieval {
    .id = "qdrant-live-retrieval-" + suffix,
    .document_id = "qdrant-live-doc-" + suffix,
    .title = "Live Retrieval",
    .content = "Qdrant live knowledge retrieval chunk.",
    .source_uri = "docs/live.md",
    .metadata = {
      { "topic", "qdrant-live" },
      { "tenant_id", suffix },
    },
  };
  knowledge_chunk other {
    .id = "qdrant-live-other-" + suffix,
    .document_id = "qdrant-live-other-doc-" + suffix,
    .content = "Other knowledge chunk.",
    .metadata = {
      { "topic", "other" },
      { "tenant_id", suffix },
    },
  };

  index.upsert_batch({ retrieval, other }, { { 1.0F, 0.0F, 0.0F }, { 0.0F, 1.0F, 0.0F } });

  knowledge_query query;
  query.text = "qdrant live retrieval";
  query.limit = 1;
  query.filters = { { "topic", "qdrant-live" } };
  query.access.tenant_id = suffix;

  const auto results = index.search(query, { 1.0F, 0.0F, 0.0F });
  require(results.size() == 1, "qdrant knowledge live integration should return one result");
  require(results.front().chunk.document_id == retrieval.document_id,
    "qdrant knowledge live integration should preserve document id");

  require(index.erase_document(retrieval.document_id),
    "qdrant knowledge live integration erase should succeed");
  index.erase_document(other.document_id);
}

void test_remote_vector_indexes_send_standard_requests() {
  auto http = std::make_shared<remote_vector_capture_http_client>();
  http->response_body = nlohmann::json {
    { "results", nlohmann::json::array({
      {
        { "score", 0.9 },
        { "chunk", {
          { "id", "chunk-1" },
          { "document_id", "doc-1" },
          { "title", "Remote" },
          { "content", "Remote vector retrieval" },
          { "source_uri", "remote://doc" },
          { "metadata", { { "topic", "rag" } } },
        } },
      },
    }) },
  }.dump();

  pgvector_knowledge_index index({
    .base_url = "http://vector.local/",
    .namespace_name = "docs",
  }, http);

  knowledge_chunk chunk {
    .id = "chunk-1",
    .document_id = "doc-1",
    .content = "Remote vector retrieval",
    .metadata = { { "topic", "rag" } },
  };
  index.upsert(chunk, { 1.0F, 0.0F, 0.0F });

  knowledge_query query;
  query.text = "Remote retrieval";
  query.limit = 1;
  query.filters = { { "topic", "rag" } };
  const auto results = index.search(query, { 1.0F, 0.0F, 0.0F });

  require(results.size() == 1,
    "remote vector knowledge index should parse standard results");
  require(http->requests.size() == 2,
    "remote vector knowledge index should send upsert and search requests");

  const auto upsert_body = nlohmann::json::parse(http->requests[0].body);
  require(upsert_body["provider"] == "pgvector",
    "pgvector index should label provider");
  require(upsert_body["namespace"] == "docs",
    "remote vector index should include namespace");
  require(http->requests[1].url == "http://vector.local/vectors/search",
    "remote vector index should normalize base URL");

  auto open_http = std::make_shared<remote_vector_capture_http_client>();
  opensearch_knowledge_index open_index({ .base_url = "http://open.local" }, open_http);
  open_index.clear();
  require(contains(open_http->requests.front().body, "opensearch"),
    "opensearch index should label provider");

  auto milvus_http = std::make_shared<remote_vector_capture_http_client>();
  milvus_knowledge_index milvus_index({ .base_url = "http://milvus.local" }, milvus_http);
  milvus_index.erase_document("doc-1");
  require(contains(milvus_http->requests.front().body, "milvus"),
    "milvus index should label provider");
}

void test_file_store_erase_persists() {
  const auto path = unique_temp_path(".jsonl");
  const auto cleanup = [&] {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(path.string() + ".tmp", ignored);
  };
  cleanup();

  {
    auto retriever = std::make_shared<knowledge_retriever>(
      std::make_shared<file_knowledge_store>(path),
      std::make_shared<in_memory_knowledge_index>(),
      std::make_shared<topic_embedding_model>(),
      knowledge_splitter({
        .max_chars = 200,
        .overlap_chars = 0,
      }));

    retriever->ingest({
      .id = "rag-doc",
      .content = "RAG retrieval persists external knowledge chunks.",
    });
    require(retriever->erase_document("rag-doc"), "erase should remove persisted document");
  }

  {
    auto retriever = std::make_shared<knowledge_retriever>(
      std::make_shared<file_knowledge_store>(path),
      std::make_shared<in_memory_knowledge_index>(),
      std::make_shared<topic_embedding_model>());

    require(retriever->rebuild_index() == 0,
      "erased document should not be restored after reload");
  }

  cleanup();
}

void test_rebuild_index_detailed_reports_partial_failures() {
  auto store = std::make_shared<in_memory_knowledge_store>();
  auto index = std::make_shared<in_memory_knowledge_index>();

  {
    auto retriever = std::make_shared<knowledge_retriever>(
      store,
      index,
      std::make_shared<topic_embedding_model>(),
      knowledge_splitter({
        .max_chars = 200,
        .overlap_chars = 0,
      }));

    retriever->ingest({
      .id = "good-doc",
      .content = "healthy knowledge chunk",
    });
    retriever->ingest({
      .id = "bad-doc",
      .content = "broken knowledge chunk",
    });
  }

  auto retriever = std::make_shared<knowledge_retriever>(
    store,
    index,
    std::make_shared<selectively_failing_embedding_model>());

  const auto rebuild = retriever->rebuild_index_detailed();
  require(rebuild.scanned == 2, "detailed rebuild should report scanned chunks");
  require(rebuild.rebuilt == 1, "detailed rebuild should continue after one failure");
  require(rebuild.errors.size() == 1, "detailed rebuild should report failed chunks");
}

void test_rebuild_index_detailed_async_reports_progress() {
  auto store = std::make_shared<in_memory_knowledge_store>();
  auto index = std::make_shared<in_memory_knowledge_index>();
  auto retriever = std::make_shared<knowledge_retriever>(
    store,
    index,
    std::make_shared<topic_embedding_model>(),
    knowledge_splitter({
      .max_chars = 200,
      .overlap_chars = 0,
    }));

  retriever->ingest({
    .id = "api-doc",
    .content = "API request contracts.",
  });
  retriever->ingest({
    .id = "rag-doc",
    .content = "RAG retrieval async rebuild.",
  });

  std::atomic<int> callbacks { 0 };
  auto task = retriever->rebuild_index_detailed_async({},
    [&](const knowledge_task_progress&) {
      ++callbacks;
    });

  const auto result = task->get();
  const auto progress = task->progress();
  require(result.scanned == 2, "async rebuild should scan stored chunks");
  require(result.rebuilt == 2, "async rebuild should rebuild stored chunks");
  require(progress.state == knowledge_task_state::completed,
    "async rebuild should complete task");
  require(progress.completed == 2 && progress.total == 2,
    "async rebuild should report final progress");
  require(callbacks.load() >= 2, "async rebuild should call progress callback");
}

void test_rebuild_index_detailed_async_retries_transient_failures() {
  auto store = std::make_shared<in_memory_knowledge_store>();
  auto index = std::make_shared<in_memory_knowledge_index>();
  {
    auto retriever = std::make_shared<knowledge_retriever>(
      store,
      index,
      std::make_shared<topic_embedding_model>(),
      knowledge_splitter({
        .max_chars = 200,
        .overlap_chars = 0,
      }));
    retriever->ingest({
      .id = "retry-doc",
      .content = "retry async rebuild",
    });
  }

  auto retriever = std::make_shared<knowledge_retriever>(
    store,
    index,
    std::make_shared<flaky_embedding_model>());

  auto task = retriever->rebuild_index_detailed_async({},
    {},
    {
      .max_retries = 1,
      .retry_backoff = std::chrono::milliseconds(1),
    });

  const auto result = task->get();
  require(result.rebuilt == 1, "async rebuild should retry transient failures");
  require(result.errors.empty(), "async rebuild retry should avoid final errors");
}

void test_ingest_batch_reports_partial_failures() {
  auto retriever = std::make_shared<knowledge_retriever>(
    std::make_shared<in_memory_knowledge_store>(),
    std::make_shared<in_memory_knowledge_index>(),
    std::make_shared<selectively_failing_embedding_model>(),
    knowledge_splitter({
      .max_chars = 200,
      .overlap_chars = 0,
    }));

  const auto result = retriever->ingest_batch({
    {
      .id = "good-doc",
      .content = "healthy knowledge chunk",
    },
    {
      .id = "bad-doc",
      .content = "broken knowledge chunk",
    },
  });

  require(result.ingested == 1, "batch ingest should count successful documents");
  require(result.errors.size() == 1, "batch ingest should report failed documents");
}

void test_ingest_batch_async_reports_progress() {
  auto retriever = std::make_shared<knowledge_retriever>(
    std::make_shared<in_memory_knowledge_store>(),
    std::make_shared<in_memory_knowledge_index>(),
    std::make_shared<topic_embedding_model>(),
    knowledge_splitter({
      .max_chars = 200,
      .overlap_chars = 0,
    }));

  std::atomic<int> callbacks { 0 };
  auto task = retriever->ingest_batch_async({
    {
      .id = "api-doc",
      .content = "API request contracts.",
    },
    {
      .id = "rag-doc",
      .content = "RAG retrieval async ingest.",
    },
  },
    [&](const knowledge_task_progress&) {
      ++callbacks;
    });

  const auto result = task->get();
  const auto progress = task->progress();
  require(result.ingested == 2, "async ingest should ingest documents");
  require(progress.state == knowledge_task_state::completed,
    "async ingest should complete task");
  require(progress.completed == 2 && progress.total == 2,
    "async ingest should report final progress");
  require(callbacks.load() >= 2, "async ingest should call progress callback");

  knowledge_query query;
  query.text = "RAG retrieval";
  query.limit = 1;
  require(retriever->retrieve(query).size() == 1,
    "async ingest should index documents for retrieval");
}

void test_ingest_batch_async_can_be_canceled() {
  auto retriever = std::make_shared<knowledge_retriever>(
    std::make_shared<in_memory_knowledge_store>(),
    std::make_shared<in_memory_knowledge_index>(),
    std::make_shared<slow_embedding_model>(),
    knowledge_splitter({
      .max_chars = 200,
      .overlap_chars = 0,
    }));

  std::vector<knowledge_document> documents;
  for (int index = 0; index < 8; ++index) {
    documents.push_back({
      .id = "doc-" + std::to_string(index),
      .content = "slow async ingest document",
    });
  }

  auto task = retriever->ingest_batch_async(std::move(documents));
  std::this_thread::sleep_for(std::chrono::milliseconds(35));
  task->request_cancel();

  const auto result = task->get();
  const auto progress = task->progress();
  require(progress.state == knowledge_task_state::canceled,
    "async ingest should report canceled state");
  require(result.ingested < 8, "async ingest cancellation should stop remaining documents");
}

void test_ingest_batch_async_retries_transient_failures() {
  auto retriever = std::make_shared<knowledge_retriever>(
    std::make_shared<in_memory_knowledge_store>(),
    std::make_shared<in_memory_knowledge_index>(),
    std::make_shared<flaky_embedding_model>(),
    knowledge_splitter({
      .max_chars = 200,
      .overlap_chars = 0,
    }));

  auto task = retriever->ingest_batch_async({
    {
      .id = "retry-doc",
      .content = "retry async ingest",
    },
  },
    {},
    {
      .max_retries = 1,
      .retry_backoff = std::chrono::milliseconds(1),
    });

  const auto result = task->get();
  require(result.ingested == 1, "async ingest should retry transient failures");
  require(result.errors.empty(), "async ingest retry should avoid final errors");
}

void test_indexing_policy_records_embedding_metadata_and_dimension() {
  auto retriever = std::make_shared<knowledge_retriever>(
    std::make_shared<in_memory_knowledge_store>(),
    std::make_shared<in_memory_knowledge_index>(),
    std::make_shared<topic_embedding_model>(),
    knowledge_splitter({
      .max_chars = 200,
      .overlap_chars = 0,
    }),
    knowledge_indexing_policy {
      .embedding_provider = "test",
      .embedding_model = "topic-3d",
      .embedding_version = "1",
      .expected_embedding_dimension = 3,
      .index_schema_version = 2,
    });

  retriever->ingest({
    .id = "rag-doc",
    .content = "RAG retrieval metadata.",
  });

  knowledge_query query;
  query.text = "RAG retrieval";
  query.limit = 1;
  const auto results = retriever->retrieve(query);
  require(results.size() == 1, "setup should retrieve indexed chunk");
  require(results.front().chunk.metadata.at("embedding_provider") == "test",
    "indexing policy should record embedding provider");
  require(results.front().chunk.metadata.at("embedding_model") == "topic-3d",
    "indexing policy should record embedding model");
  require(results.front().chunk.metadata.at("embedding_dimension") == "3",
    "indexing policy should record embedding dimension");
  require(results.front().chunk.metadata.at("index_schema_version") == "2",
    "indexing policy should record schema version");

  auto mismatched = std::make_shared<knowledge_retriever>(
    std::make_shared<in_memory_knowledge_store>(),
    std::make_shared<in_memory_knowledge_index>(),
    std::make_shared<topic_embedding_model>(),
    knowledge_splitter(),
    knowledge_indexing_policy {
      .expected_embedding_dimension = 4,
    });

  bool threw = false;
  try {
    mismatched->ingest({
      .id = "bad-dimension",
      .content = "RAG retrieval",
    });
  }
  catch (const std::exception&) {
    threw = true;
  }
  require(threw, "indexing policy should reject unexpected embedding dimensions");
}

void test_retriever_returns_relevant_chunk() {
  auto retriever = make_retriever({
    .max_chars = 200,
    .overlap_chars = 0,
  });

  retriever->ingest({
    .id = "api-doc",
    .title = "API Guide",
    .content = "The API guide explains request and response contracts.",
    .source_uri = "docs/api.md",
    .metadata = { { "topic", "api" } },
  });
  retriever->ingest({
    .id = "rag-doc",
    .title = "Knowledge Retrieval",
    .content = "Retrieval augmented generation searches external documents.",
    .source_uri = "docs/rag.md",
    .metadata = { { "topic", "rag" } },
  });

  knowledge_query query;
  query.text = "How does RAG retrieval work?";
  query.limit = 1;

  const auto results = retriever->retrieve(query);
  require(results.size() == 1, "retriever should return one result");
  require(results.front().chunk.document_id == "rag-doc",
    "retriever should rank the retrieval document first");
  require(results.front().vector_score > 0.9,
    "retriever should expose raw vector score");
  require(results.front().lexical_score > 0.0,
    "retriever should expose raw lexical score");

  query.filters = { { "topic", "api" } };
  const auto filtered = retriever->retrieve(query);
  require(filtered.size() == 1, "retriever should apply metadata filters");
  require(filtered.front().chunk.document_id == "api-doc",
    "metadata filter should restrict retrieval to API topic");
}

void test_retriever_applies_access_scope() {
  auto retriever = make_retriever({
    .max_chars = 200,
    .overlap_chars = 0,
  });

  retriever->ingest({
    .id = "tenant-a",
    .content = "RAG retrieval private tenant A.",
    .metadata = {
      { "tenant_id", "tenant-a" },
      { "allowed_roles", "admin,reader" },
    },
  });
  retriever->ingest({
    .id = "tenant-b",
    .content = "RAG retrieval private tenant B.",
    .metadata = {
      { "tenant_id", "tenant-b" },
      { "allowed_roles", "admin" },
    },
  });

  knowledge_query query;
  query.text = "RAG retrieval private";
  query.limit = 10;
  query.access.tenant_id = "tenant-a";
  query.access.roles = { "reader" };

  const auto results = retriever->retrieve(query);
  require(results.size() == 1, "access scope should filter tenant and roles");
  require(results.front().chunk.document_id == "tenant-a",
    "access scope should return only authorized document");
}

void test_bm25_reranker_promotes_exact_lexical_match() {
  auto retriever = make_retriever({
    .max_chars = 200,
    .overlap_chars = 0,
  });

  retriever->ingest({
    .id = "generic-rag",
    .title = "Retrieval Guide",
    .content = "RAG retrieval retrieval retrieval overview.",
  });
  retriever->ingest({
    .id = "timeout-rag",
    .title = "Timeout Guide",
    .content = "RAG retry timeout backoff configuration.",
  });
  retriever->set_reranker(std::make_shared<bm25_knowledge_reranker>(
    bm25_knowledge_reranker_policy {
      .existing_score_weight = 0.0,
      .bm25_weight = 1.0,
    }));

  knowledge_query query;
  query.text = "timeout backoff";
  query.limit = 1;
  query.candidate_limit = 2;
  query.vector_weight = 0.0;
  query.lexical_weight = 0.0;

  const auto results = retriever->retrieve(query);
  require(results.size() == 1, "bm25 reranker should return one result");
  require(results.front().chunk.document_id == "timeout-rag",
    "bm25 reranker should promote exact lexical match");
  require(results.front().lexical_score > 0.0,
    "bm25 reranker should expose bm25 lexical score");
}

void test_query_rewriter_enables_multi_query_retrieval() {
  auto retriever = make_retriever({
    .max_chars = 200,
    .overlap_chars = 0,
  });
  retriever->set_query_rewriter(
    std::make_shared<static_knowledge_query_rewriter>(
      std::vector<std::string> { "timeout backoff" }));

  retriever->ingest({
    .id = "timeout-doc",
    .content = "Timeout backoff configuration prevents retry storms.",
  });

  knowledge_query query;
  query.text = "resilience policy";
  query.limit = 1;
  query.vector_weight = 1.0;
  query.lexical_weight = 0.25;

  const auto report = retriever->retrieve_detailed(query);
  require(report.results.size() == 1,
    "query rewriter should retrieve with alternate query text");
  require(report.results.front().chunk.document_id == "timeout-doc",
    "multi-query retrieval should return rewritten-query result");
  require(report.trace.rewritten_query_count == 1,
    "retrieval trace should count rewritten queries");
}

void test_candidate_limit_applies_without_reranker() {
  auto retriever = make_retriever({
    .max_chars = 80,
    .overlap_chars = 0,
    .include_document_summary_chunk = true,
  });

  retriever->ingest({
    .id = "patterns-doc",
    .title = "Patterns",
    .content = "Overview of agentic design patterns.\n"
               "1. Prompt Chaining\n"
               "2. Tool Use\n"
               "3. Knowledge Retrieval\n"
               "Long body text that creates another chunk.",
  });

  knowledge_query query;
  query.text = "What are the main patterns?";
  query.limit = 1;
  query.candidate_limit = 8;

  const auto report = retriever->retrieve_detailed(query);
  require(report.trace.first_stage_count > 1,
    "candidate limit should fetch broader candidates without reranker");
  require(report.results.size() == 1,
    "retriever should still return final limit without reranker");
}

void test_http_query_rewriter_calls_remote_service() {
  auto http = std::make_shared<remote_vector_capture_http_client>();
  http->response_body = R"({"rewrites":["timeout backoff","retry policy","ignored extra"]})";

  http_knowledge_query_rewriter rewriter({
    .endpoint_url = "http://rewriter.local/rewrite",
    .api_key = "token",
    .timeout_ms = 1234,
    .max_rewrites = 2,
  }, http);

  const auto rewrites = rewriter.rewrite("resilience");
  require(rewrites.size() == 2, "http query rewriter should honor max rewrites");
  require(rewrites.front() == "timeout backoff",
    "http query rewriter should parse rewrite strings");
  require(http->requests.size() == 1, "http query rewriter should send one request");
  require(http->requests.front().method == "POST",
    "http query rewriter should use POST");

  const auto body = nlohmann::json::parse(http->requests.front().body);
  require(body["query"] == "resilience",
    "http query rewriter should send query text");
  require(body["max_rewrites"] == 2,
    "http query rewriter should send rewrite limit");

  bool has_auth = false;
  for (const auto& [key, value] : http->requests.front().headers) {
    if (key == "Authorization" && value == "Bearer token") {
      has_auth = true;
    }
  }
  require(has_auth, "http query rewriter should attach bearer token");
}

void test_llm_query_rewriter_parses_json_array_response() {
  llm_knowledge_query_rewriter rewriter(
    std::make_shared<fake_llm_client>("Here is JSON: [\"agent patterns\", \"tool use\"]"),
    {
      .max_rewrites = 1,
    });

  const auto rewrites = rewriter.rewrite("What patterns are described?");
  require(rewrites.size() == 1, "llm query rewriter should honor max rewrites");
  require(rewrites.front() == "agent patterns",
    "llm query rewriter should parse JSON array from response");
}

void test_mmr_reranker_promotes_diverse_results() {
  std::vector<knowledge_result> candidates {
    {
      .chunk = { .id = "a", .document_id = "doc-a", .content = "retry retry timeout backoff" },
      .score = 1.0,
    },
    {
      .chunk = { .id = "b", .document_id = "doc-b", .content = "retry retry timeout backoff" },
      .score = 0.95,
    },
    {
      .chunk = { .id = "c", .document_id = "doc-c", .content = "circuit breaker isolation" },
      .score = 0.7,
    },
  };

  knowledge_query query;
  query.text = "retry timeout";
  query.limit = 2;
  mmr_knowledge_reranker reranker({
    .relevance_weight = 0.5,
    .diversity_weight = 0.8,
  });

  const auto results = reranker.rerank(query, std::move(candidates));
  require(results.size() == 2, "MMR reranker should honor limit");
  require(results.front().chunk.id == "a", "MMR should keep strongest first result");
  require(results[1].chunk.id == "c", "MMR should promote diverse second result");
}

void test_cross_encoder_reranker_uses_model_scores() {
  std::vector<knowledge_result> candidates {
    {
      .chunk = { .id = "generic", .document_id = "doc-a", .content = "generic answer" },
      .score = 1.0,
    },
    {
      .chunk = { .id = "preferred", .document_id = "doc-b", .content = "preferred answer" },
      .score = 0.1,
    },
  };

  knowledge_query query;
  query.text = "best answer";
  query.limit = 1;
  cross_encoder_knowledge_reranker reranker(
    std::make_shared<keyword_cross_encoder_scorer>(),
    { .existing_score_weight = 0.0, .model_score_weight = 1.0 });

  const auto results = reranker.rerank(query, std::move(candidates));
  require(results.size() == 1, "cross-encoder reranker should honor limit");
  require(results.front().chunk.id == "preferred",
    "cross-encoder reranker should rank by model score");

  callback_cross_encoder_knowledge_scorer callback_scorer(
    [](const knowledge_query&, const knowledge_result& candidate) {
      return candidate.chunk.id == "preferred" ? 3.0 : 0.0;
    });
  require(callback_scorer.score(query, results.front()) == 3.0,
    "callback cross-encoder scorer should support LLM-backed scoring hooks");
}

void test_http_cross_encoder_scorer_calls_remote_service() {
  auto http = std::make_shared<reranker_capture_http_client>();
  http_cross_encoder_knowledge_scorer scorer({
    .endpoint_url = "http://reranker.local/score",
    .api_key = "token",
    .timeout_ms = 1234,
  }, http);

  knowledge_query query;
  query.text = "best result";
  knowledge_result candidate {
    .chunk = {
      .id = "chunk-1",
      .document_id = "doc-1",
      .title = "Candidate",
      .content = "Remote reranker candidate.",
      .source_uri = "docs/rerank.md",
      .metadata = { { "topic", "rerank" } },
    },
    .score = 0.5,
    .vector_score = 0.4,
    .lexical_score = 0.1,
  };

  const auto score = scorer.score(query, candidate);
  require(score == 4.2, "http cross encoder scorer should parse score response");
  require(http->requests.size() == 1, "http scorer should send one request");
  require(http->requests.front().method == "POST",
    "http scorer should use POST");
  require(http->requests.front().url == "http://reranker.local/score",
    "http scorer should use configured endpoint");

  const auto body = nlohmann::json::parse(http->requests.front().body);
  require(body["query"] == "best result", "http scorer should send query text");
  require(body["candidate"]["id"] == "chunk-1",
    "http scorer should send candidate chunk");

  bool has_auth = false;
  for (const auto& [key, value] : http->requests.front().headers) {
    if (key == "Authorization" && value == "Bearer token") {
      has_auth = true;
    }
  }
  require(has_auth, "http scorer should attach bearer token");
}

void test_retrieve_detailed_reports_trace_metrics() {
  auto retriever = make_retriever({
    .max_chars = 200,
    .overlap_chars = 0,
  });

  retriever->ingest({
    .id = "generic-rag",
    .content = "RAG retrieval overview.",
  });
  retriever->ingest({
    .id = "timeout-rag",
    .content = "RAG retry timeout backoff configuration.",
  });
  retriever->set_reranker(std::make_shared<bm25_knowledge_reranker>());

  knowledge_query query;
  query.text = "timeout backoff";
  query.limit = 1;
  query.candidate_limit = 2;

  const auto report = retriever->retrieve_detailed(query);
  require(report.results.size() == 1, "detailed retrieval should return final results");
  require(report.trace.query_text == "timeout backoff",
    "retrieval trace should include query text");
  require(report.trace.requested_limit == 1,
    "retrieval trace should include requested limit");
  require(report.trace.candidate_limit == 2,
    "retrieval trace should include candidate limit");
  require(report.trace.first_stage_count == 2,
    "retrieval trace should include first-stage candidate count");
  require(report.trace.after_access_filter_count == 2,
    "retrieval trace should include access-filtered count");
  require(report.trace.final_count == 1,
    "retrieval trace should include final result count");
  require(report.trace.used_reranker,
    "retrieval trace should record reranker usage");
  require(report.trace.total_ms >= 0.0,
    "retrieval trace should include total latency");
}

void test_retriever_publishes_observability_events() {
  auto retriever = make_retriever({
    .max_chars = 200,
    .overlap_chars = 0,
  });
  auto sink = std::make_shared<in_memory_knowledge_event_sink>();
  retriever->set_event_sink(sink);

  retriever->ingest({
    .id = "rag-doc",
    .content = "RAG retrieval emits traceable observability events.",
  });

  knowledge_query query;
  query.text = "RAG retrieval";
  query.limit = 1;
  const auto report = retriever->retrieve_detailed(query);
  const auto rebuild = retriever->rebuild_index_detailed();

  require(!report.trace.trace_id.empty(), "retrieval trace should include trace id");
  require(rebuild.rebuilt == 1, "rebuild should keep the ingested chunk indexed");

  auto failing_retriever = std::make_shared<knowledge_retriever>(
    std::make_shared<in_memory_knowledge_store>(),
    std::make_shared<in_memory_knowledge_index>(),
    std::make_shared<selectively_failing_embedding_model>(),
    knowledge_splitter({
      .max_chars = 200,
      .overlap_chars = 0,
    }));
  failing_retriever->set_event_sink(sink);
  bool failed_ingest_threw = false;
  try {
    failing_retriever->ingest({
      .id = "broken-doc",
      .content = "broken embedding input",
    });
  }
  catch (const std::runtime_error&) {
    failed_ingest_threw = true;
  }
  require(failed_ingest_threw, "failed ingest should still throw");

  const auto events = sink->events();
  auto has_event = [&](const std::string& name) {
    for (const auto& event : events) {
      if (event.name == name) {
        return true;
      }
    }
    return false;
  };

  require(has_event("knowledge.ingest.start"),
    "observability sink should receive ingest start");
  require(has_event("knowledge.ingest.complete"),
    "observability sink should receive ingest completion");
  require(has_event("knowledge.retrieve.start"),
    "observability sink should receive retrieve start");
  require(has_event("knowledge.retrieve.complete"),
    "observability sink should receive retrieve completion");
  require(has_event("knowledge.rebuild.start"),
    "observability sink should receive rebuild start");
  require(has_event("knowledge.rebuild.complete"),
    "observability sink should receive rebuild completion");
  require(has_event("knowledge.ingest.failed"),
    "observability sink should receive failed ingest");

  bool retrieve_complete_matches_trace = false;
  for (const auto& event : events) {
    if (event.name == "knowledge.retrieve.complete" &&
        event.trace_id == report.trace.trace_id &&
        event.attributes.at("final_count") == "1") {
      retrieve_complete_matches_trace = true;
    }
  }
  require(retrieve_complete_matches_trace,
    "retrieve completion event should share trace id and final count");
}

void test_retrieval_cache_hits_and_invalidates() {
  auto retriever = make_retriever({
    .max_chars = 200,
    .overlap_chars = 0,
  });
  retriever->set_retrieval_cache(std::make_shared<in_memory_knowledge_retrieval_cache>());

  retriever->ingest({
    .id = "rag-doc",
    .content = "RAG retrieval cache document.",
  });

  knowledge_query query;
  query.text = "RAG retrieval";
  query.limit = 1;

  const auto first = retriever->retrieve_detailed(query);
  const auto second = retriever->retrieve_detailed(query);
  require(!first.trace.cache_hit, "first retrieval should miss cache");
  require(second.trace.cache_hit, "second retrieval should hit cache");
  require(second.results.size() == 1, "cached retrieval should return results");

  retriever->ingest({
    .id = "api-doc",
    .content = "API cache invalidation document.",
  });
  const auto after_ingest = retriever->retrieve_detailed(query);
  require(!after_ingest.trace.cache_hit, "ingest should invalidate retrieval cache");
}

void test_retrieval_cache_eviction_and_ttl() {
  in_memory_knowledge_retrieval_cache cache(2, std::chrono::milliseconds(30));
  std::vector<knowledge_result> result_a {
    { .chunk = { .id = "a", .document_id = "a" }, .score = 1.0 },
  };
  std::vector<knowledge_result> output;

  cache.put("a", result_a);
  cache.put("b", { { .chunk = { .id = "b", .document_id = "b" }, .score = 1.0 } });
  require(cache.get("a", output), "LRU cache should retrieve existing item");

  cache.put("c", { { .chunk = { .id = "c", .document_id = "c" }, .score = 1.0 } });
  require(cache.get("a", output), "LRU cache should keep recently used item");
  require(!cache.get("b", output), "LRU cache should evict least recently used item");

  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  require(!cache.get("a", output), "retrieval cache should expire entries by TTL");
}

void test_cached_reranker_reuses_results() {
  auto inner = std::make_shared<counting_knowledge_reranker>();
  cached_knowledge_reranker reranker(inner);

  knowledge_query query;
  query.text = "RAG retrieval";
  query.limit = 1;
  std::vector<knowledge_result> candidates {
    {
      .chunk = { .id = "chunk-1", .document_id = "doc-1", .content = "RAG retrieval" },
      .score = 1.0,
    },
  };

  const auto first = reranker.rerank(query, candidates);
  const auto second = reranker.rerank(query, candidates);
  require(first.size() == 1 && second.size() == 1,
    "cached reranker should return reranked results");
  require(inner->calls == 1,
    "cached reranker should avoid repeated inner reranker calls");
}

void test_cached_reranker_eviction_and_ttl() {
  auto inner = std::make_shared<counting_knowledge_reranker>();
  cached_knowledge_reranker reranker(inner, 1, std::chrono::milliseconds(30));

  knowledge_query query;
  query.text = "RAG retrieval";
  query.limit = 1;
  std::vector<knowledge_result> first_candidate {
    { .chunk = { .id = "chunk-1", .document_id = "doc-1" }, .score = 1.0 },
  };
  std::vector<knowledge_result> second_candidate {
    { .chunk = { .id = "chunk-2", .document_id = "doc-2" }, .score = 1.0 },
  };

  reranker.rerank(query, first_candidate);
  reranker.rerank(query, second_candidate);
  reranker.rerank(query, first_candidate);
  require(inner->calls == 3, "cached reranker should evict least recently used entry");

  reranker.rerank(query, first_candidate);
  require(inner->calls == 3, "cached reranker should hit cache before TTL");
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  reranker.rerank(query, first_candidate);
  require(inner->calls == 4, "cached reranker should expire entries by TTL");
}

void test_knowledge_metrics_export_prometheus_and_otel() {
  prometheus_knowledge_event_sink prometheus;
  otel_knowledge_event_sink otel;

  knowledge_event event {
    .trace_id = "trace-1",
    .name = "knowledge.retrieve.complete",
    .attributes = {
      { "final_count", "2" },
      { "total_ms", "12.5" },
    },
  };
  prometheus.publish(event);
  otel.publish(event);

  const auto scrape = prometheus.scrape();
  require(contains(scrape, "wuwe_knowledge_events_total"),
    "prometheus sink should export event counter");
  require(contains(scrape, "knowledge.retrieve.complete"),
    "prometheus sink should label event names");
  require(contains(scrape, "wuwe_knowledge_event_latency_ms_sum"),
    "prometheus sink should export latency sum");
  require(contains(scrape, "wuwe_knowledge_retrieval_results_sum"),
    "prometheus sink should export result count sum");

  const auto spans = otel.spans();
  require(spans.size() == 1 && spans.front().trace_id == "trace-1",
    "otel sink should preserve trace id");
  require(spans.front().attributes.at("final_count") == "2",
    "otel sink should preserve attributes");
}

void test_context_augments_request_with_citations() {
  auto retriever = make_retriever({
    .max_chars = 200,
    .overlap_chars = 0,
  });

  retriever->ingest({
    .id = "rag-doc",
    .title = "Knowledge Retrieval",
    .content = "Retrieval augmented generation injects cited chunks into the model request.",
    .source_uri = "docs/rag.md",
  });

  knowledge_context context(retriever, {
    .max_context_chars = 1000,
    .max_results = 2,
  });

  llm_request request;
  request.messages.push_back({ .role = "system", .content = "You answer with citations." });
  request.messages.push_back({ .role = "user", .content = "Explain RAG." });

  const auto augmented = context.augment(std::move(request), "RAG retrieval");
  require(augmented.messages.size() == 3, "knowledge context should inject one message");
  require(augmented.messages[1].role == "system",
    "knowledge context should insert after existing system messages");
  require(contains(augmented.messages[1].content, "Relevant knowledge:"),
    "knowledge block should use default header");
  require(contains(augmented.messages[1].content, "[1] docs/rag.md"),
    "knowledge block should include citations");
}

void test_result_processor_dedupes_and_merges_adjacent_chunks() {
  knowledge_result_processor processor({
    .dedupe_chunks = true,
    .merge_adjacent_chunks = true,
    .max_merged_chars = 200,
  });

  knowledge_chunk first {
    .id = "doc#1",
    .document_id = "doc",
    .content = "First cited chunk.",
    .start_offset = 0,
    .end_offset = 18,
    .start_line = 1,
    .end_line = 1,
    .source_uri = "docs/guide.md",
  };
  knowledge_chunk second {
    .id = "doc#2",
    .document_id = "doc",
    .content = "Second cited chunk.",
    .start_offset = 18,
    .end_offset = 38,
    .start_line = 2,
    .end_line = 2,
    .source_uri = "docs/guide.md",
  };

  const auto results = processor.process({
    { .chunk = first, .score = 0.9 },
    { .chunk = first, .score = 0.8 },
    { .chunk = second, .score = 0.7 },
  });

  require(results.size() == 1,
    "result processor should dedupe duplicate chunks and merge adjacent chunks");
  require(contains(results.front().chunk.content, "First cited chunk."),
    "merged result should include first chunk");
  require(contains(results.front().chunk.content, "Second cited chunk."),
    "merged result should include second chunk");
  require(results.front().chunk.end_line == 2, "merged result should extend line range");
}

void test_context_merges_adjacent_chunks_before_injection() {
  auto retriever = make_retriever({
    .max_chars = 36,
    .overlap_chars = 0,
    .respect_markdown_headings = false,
    .prefer_paragraph_boundaries = false,
  });

  retriever->ingest({
    .id = "rag-doc",
    .title = "Retrieval Guide",
    .content = "RAG retrieval first sentence. RAG retrieval second sentence.",
    .source_uri = "docs/rag.md",
  });

  knowledge_context context(retriever, {
    .max_context_chars = 1000,
    .max_results = 4,
  });

  const auto block = context.build_context_block("RAG retrieval");
  require(contains(block, "[1] docs/rag.md"), "context should include citation");
  require(!contains(block, "[2] docs/rag.md"),
    "context should merge adjacent chunks into one citation");
  require(contains(block, "second sentence"), "merged context should include adjacent content");
}

void test_context_expands_retrieved_chunks_with_neighbors() {
  auto retriever = make_retriever({
    .max_chars = 24,
    .overlap_chars = 0,
    .respect_markdown_headings = false,
    .prefer_paragraph_boundaries = false,
  });

  retriever->ingest({
    .id = "guide",
    .title = "Guide",
    .content = "Before context paragraph. targetneedle middle paragraph. After context paragraph.",
    .source_uri = "docs/guide.md",
  });

  knowledge_context context(retriever, {
    .max_context_chars = 1000,
    .max_results = 1,
    .surrounding_chunks_before = 1,
    .surrounding_chunks_after = 1,
  });

  const auto block = context.build_context_block("targetneedle");
  require(contains(block, "Before context"),
    "context expansion should include preceding sibling chunk");
  require(contains(block, "targetneedle"),
    "context expansion should include retrieved chunk");
  require(contains(block, "After context"),
    "context expansion should include following sibling chunk");
}

void test_context_truncates_oversized_chunks() {
  auto retriever = make_retriever({
    .max_chars = 2000,
    .overlap_chars = 0,
  });

  retriever->ingest({
    .id = "large-doc",
    .title = "Large Guide",
    .content = "RAG retrieval " + std::string(1500, 'x'),
    .source_uri = "docs/large.md",
  });

  knowledge_context context(retriever, {
    .max_context_chars = 240,
    .max_results = 1,
  });

  const auto block = context.build_context_block("RAG retrieval");
  require(!block.empty(), "context should include oversized chunks by truncating");
  require(block.size() <= 260, "truncated context should stay near the budget");
  require(contains(block, "docs/large.md"), "truncated context should preserve citation");
  require(contains(block, "..."), "truncated context should mark truncated content");
}

void test_hybrid_retrieval_uses_lexical_score_and_threshold() {
  auto retriever = make_retriever({
    .max_chars = 200,
    .overlap_chars = 0,
  });

  retriever->ingest({
    .id = "api-doc",
    .title = "API Guide",
    .content = "The request schema includes retry and timeout options.",
    .source_uri = "docs/api.md",
  });
  retriever->ingest({
    .id = "other-doc",
    .title = "Other Guide",
    .content = "General operational notes.",
    .source_uri = "docs/other.md",
  });

  knowledge_query query;
  query.text = "request schema timeout";
  query.limit = 1;
  query.vector_weight = 0.0;
  query.lexical_weight = 1.0;

  const auto results = retriever->retrieve(query);
  require(results.size() == 1, "lexical-only retrieval should return one result");
  require(results.front().chunk.document_id == "api-doc",
    "lexical-only retrieval should rank matching text first");
  require(results.front().vector_score >= 0.0,
    "lexical-only retrieval should still report vector score");
  require(results.front().lexical_score > 0.0,
    "lexical-only retrieval should report lexical score");

  query.minimum_score = 1.1;
  require(retriever->retrieve(query).empty(),
    "minimum_score should filter weak retrieval results");
}

void test_erase_document_removes_results() {
  auto retriever = make_retriever({
    .max_chars = 200,
    .overlap_chars = 0,
  });

  retriever->ingest({
    .id = "rag-doc",
    .content = "RAG retrieval searches external knowledge.",
  });

  knowledge_query query;
  query.text = "RAG retrieval";
  require(!retriever->retrieve(query).empty(), "setup should retrieve ingested document");

  require(retriever->erase_document("rag-doc"), "erase_document should remove document");
  require(retriever->retrieve(query).empty(), "erased document should not be retrieved");
}

void test_knowledge_tool_provider_searches_with_citations() {
  auto retriever = make_retriever({
    .max_chars = 200,
    .overlap_chars = 0,
  });

  retriever->ingest({
    .id = "rag-doc",
    .title = "Retrieval Guide",
    .content = "# Retrieval\nRAG retrieval searches cited documents.\n",
    .source_uri = "docs/rag.md",
    .metadata = { { "topic", "rag" }, { "collection", "public" } },
  });
  retriever->ingest({
    .id = "internal-rag-doc",
    .title = "Internal Retrieval Guide",
    .content = "# Retrieval\nRAG retrieval internal runbook.\n",
    .source_uri = "docs/internal-rag.md",
    .metadata = { { "topic", "rag" }, { "collection", "internal" } },
  });

  knowledge_tool_provider provider(*retriever, {
    .max_search_results = 2,
  });

  const auto tools = provider.tools();
  require(tools.size() == 1 && tools.front().name == "search_knowledge",
    "knowledge tool provider should expose search_knowledge");

  const auto result = provider.invoke(
    "search_knowledge",
    R"({"content":"RAG retrieval","topic":"rag","filters":{"collection":"public"},"limit":5})");
  require(!result.error_code, "search_knowledge should succeed");

  const auto json = nlohmann::json::parse(result.content);
  require(json.is_array() && json.size() == 1,
    "search_knowledge should apply metadata filters");
  require(json[0]["source_uri"] == "docs/rag.md",
    "search_knowledge should include source uri");
  require(json[0].contains("vector_score"),
    "search_knowledge should include vector score breakdown");
  require(json[0].contains("lexical_score"),
    "search_knowledge should include lexical score breakdown");
  require(json[0].contains("start_line"), "search_knowledge should include line metadata");
}

void test_evaluate_knowledge_retrieval_reports_recall_and_mrr() {
  auto retriever = make_retriever({
    .max_chars = 200,
    .overlap_chars = 0,
  });

  retriever->ingest({
    .id = "api-doc",
    .title = "API Guide",
    .content = "The API guide explains request and response contracts.",
    .source_uri = "docs/api.md",
  });
  retriever->ingest({
    .id = "rag-doc",
    .title = "Retrieval Guide",
    .content = "RAG retrieval searches cited documents.",
    .source_uri = "docs/rag.md",
  });

  const auto report = evaluate_knowledge_retrieval(*retriever, {
    {
      .name = "api",
      .query = "API request contracts",
      .expected_document_ids = { "api-doc" },
      .limit = 1,
    },
    {
      .name = "rag",
      .query = "RAG retrieval",
      .expected_document_ids = { "rag-doc" },
      .limit = 1,
    },
  });

  require(report.total == 2, "knowledge eval should count cases");
  require(report.hits == 2, "knowledge eval should count hits");
  require(report.recall_at_k == 1.0, "knowledge eval should report recall at k");
  require(report.mean_reciprocal_rank == 1.0, "knowledge eval should report MRR");
  require(report.cases.size() == 2, "knowledge eval should preserve case results");
}

void test_knowledge_eval_loads_cases_and_reports_terms() {
  const auto path = unique_temp_path(".json");
  {
    std::ofstream output(path, std::ios::binary);
    output << R"([
      {
        "name": "rag",
        "query": "RAG retrieval",
        "expected_document_ids": ["rag-doc"],
        "expected_terms": ["cited documents"],
        "limit": 1
      }
    ])";
  }

  const auto cleanup = [&] {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
  };

  auto retriever = make_retriever({
    .max_chars = 200,
    .overlap_chars = 0,
  });
  retriever->ingest({
    .id = "rag-doc",
    .content = "RAG retrieval searches cited documents.",
  });

  const auto cases = load_knowledge_eval_cases(path);
  const auto report = evaluate_knowledge_retrieval(*retriever, cases);
  const auto json = knowledge_eval_result_to_json(report);

  require(cases.size() == 1, "knowledge eval loader should load JSON cases");
  require(report.term_hits == 1, "knowledge eval should count expected term hits");
  require(json["cases"][0]["terms_hit"] == true,
    "knowledge eval JSON should include per-case term result");

  cleanup();
}

void test_knowledge_eval_runs_offline_fixture() {
  auto retriever = make_retriever({
    .max_chars = 180,
    .overlap_chars = 0,
    .include_document_summary_chunk = true,
  });

  const auto documents =
    knowledge_document_loader::make_default().load(fixture_path("knowledge_eval_corpus"));
  const auto ingest = retriever->ingest_batch(documents);
  require(ingest.errors.empty(), "offline eval fixture should ingest without errors");

  const auto cases = load_knowledge_eval_cases(fixture_path("knowledge_eval_cases.json"));
  const auto report = evaluate_knowledge_retrieval(*retriever, cases);

  require(report.total == 2, "offline eval fixture should load two cases");
  require(report.hits == 2, "offline eval fixture should hit expected documents");
  require(report.term_hits == 2, "offline eval fixture should hit expected terms");
  require(report.recall_at_k == 1.0, "offline eval fixture should report full recall");
  require(report.term_recall == 1.0, "offline eval fixture should report full term recall");
}

void test_rag_service_uploads_document_and_answers() {
  const auto path = unique_temp_path(".md");
  {
    std::ofstream output(path, std::ios::binary);
    output << "# Retrieval\nRAG retrieval searches cited documents.";
  }

  const auto cleanup = [&] {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
  };

  auto retriever = make_retriever({
    .max_chars = 200,
    .overlap_chars = 0,
  });
  knowledge_rag_service service(
    retriever,
    knowledge_document_loader::make_default(),
    std::make_shared<fake_llm_client>("RAG searches cited documents [1]."));

  const auto upload = service.upload_document(path, {
    .metadata = { { "collection", "service-test" } },
  });
  require(upload.documents == 1, "rag service should load one uploaded document");
  require(upload.ingest.ingested == 1, "rag service should ingest uploaded document");
  require(upload.stages.size() == 2, "rag service should report load and ingest stages");
  require(upload.total_ms >= 0.0, "rag service should report total upload time");

  knowledge_policy policy;
  policy.max_results = 1;
  const auto answer = service.ask({
    .query = "RAG retrieval",
    .policy = policy,
  });
  require(answer.citations.size() == 1, "rag service should return citations");
  require(contains(answer.context_block, "[1]"),
    "rag service should build cited context block");
  require(answer.answer.content == "RAG searches cited documents [1].",
    "rag service should use configured llm client");
  require(answer.total_ms >= 0.0, "rag service should report ask timing");

  cleanup();
}

void test_knowledge_benchmark_reports_latency() {
  auto retriever = make_retriever({
    .max_chars = 200,
    .overlap_chars = 0,
  });
  retriever->ingest({
    .id = "rag-doc",
    .content = "RAG retrieval benchmark document.",
  });

  const auto report = benchmark_knowledge_retrieval(*retriever, {
    { .query = "RAG retrieval", .limit = 1 },
    { .query = "benchmark document", .limit = 1 },
    { .query = "RAG benchmark", .limit = 1 },
  }, {
    .concurrency = 2,
  });

  require(report.query_count == 3, "benchmark should count queries");
  require(report.total_results >= 1, "benchmark should count returned results");
  require(report.average_ms >= 0.0, "benchmark should report average latency");
  require(report.p95_ms >= 0.0, "benchmark should report p95 latency");
  require(report.p99_ms >= 0.0, "benchmark should report p99 latency");
  require(report.max_ms >= 0.0, "benchmark should report max latency");
  require(report.latency_samples_ms.size() == 3,
    "benchmark should keep latency samples by default");

  const auto json = nlohmann::json::parse(knowledge_benchmark_report_to_json(report));
  require(json["query_count"] == 3, "benchmark JSON should include query count");
  require(json.contains("p95_ms"), "benchmark JSON should include p95");
}

void test_knowledge_benchmark_loads_query_file() {
  const auto path = unique_temp_path(".queries");
  {
    std::ofstream output(path, std::ios::binary);
    output << "# comments are ignored\n";
    output << "RAG retrieval\t3\n";
    output << "API contracts\n";
  }

  const auto cleanup = [&] {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
  };

  const auto cases = load_knowledge_benchmark_cases(path, 5);
  require(cases.size() == 2, "benchmark query loader should skip comments");
  require(cases[0].query == "RAG retrieval", "benchmark query loader should parse query text");
  require(cases[0].limit == 3, "benchmark query loader should parse tab limit");
  require(cases[1].limit == 5, "benchmark query loader should apply default limit");

  cleanup();
}

void test_knowledge_store_migration_audits_and_reindexes_target() {
  auto source_store = std::make_shared<in_memory_knowledge_store>();
  auto source = std::make_shared<knowledge_retriever>(
    source_store,
    std::make_shared<in_memory_knowledge_index>(),
    std::make_shared<topic_embedding_model>(),
    knowledge_splitter({
      .max_chars = 200,
      .overlap_chars = 0,
    }),
    knowledge_indexing_policy {
      .embedding_provider = "test",
      .embedding_model = "topic",
      .expected_embedding_dimension = 3,
      .index_schema_version = 2,
    });

  source->ingest({
    .id = "rag-doc",
    .title = "Retrieval Guide",
    .content = "RAG retrieval migration document.",
    .source_uri = "docs/rag.md",
  });

  const auto source_audit = audit_knowledge_store(*source_store, {
    .expected_embedding_dimension = 3,
    .expected_index_schema_version = 2,
  });
  require(source_audit.documents == 1, "store audit should count documents");
  require(source_audit.chunks == 1, "store audit should count chunks");
  require(source_audit.warnings.empty(), "store audit should accept matching metadata");

  auto target_store = std::make_shared<in_memory_knowledge_store>();
  auto target = std::make_shared<knowledge_retriever>(
    target_store,
    std::make_shared<in_memory_knowledge_index>(),
    std::make_shared<topic_embedding_model>(),
    knowledge_splitter({
      .max_chars = 200,
      .overlap_chars = 0,
    }),
    knowledge_indexing_policy {
      .embedding_provider = "test",
      .embedding_model = "topic",
      .expected_embedding_dimension = 3,
      .index_schema_version = 2,
    });

  const auto migration = migrate_knowledge_store(*source_store, *target_store, *target, {
    .clear_target = true,
  }, {
    .expected_embedding_dimension = 3,
    .expected_index_schema_version = 2,
  });

  require(migration.source_documents == 1, "migration should report source document count");
  require(migration.ingest.ingested == 1, "migration should ingest source documents");
  require(migration.target_audit.documents == 1, "migration should populate target store");
  require(migration.target_audit.chunks == 1, "migration should populate target chunks");

  knowledge_query query;
  query.text = "RAG retrieval";
  query.limit = 1;
  const auto results = target->retrieve(query);
  require(results.size() == 1, "migration should rebuild target retrieval index");
  require(results.front().chunk.document_id == "rag-doc",
    "migration should preserve document identity");
}

void test_knowledge_pipeline_builder_local_preset() {
  auto pipeline = knowledge_pipeline::make()
                    .local()
                    .with_embedding_model(std::make_shared<topic_embedding_model>())
                    .with_splitter(knowledge_splitter({
                      .max_chars = 200,
                      .overlap_chars = 0,
                    }))
                    .with_context_policy({
                      .max_context_chars = 1000,
                      .max_results = 1,
                    })
                    .build();

  pipeline.retriever().ingest({
    .id = "rag-doc",
    .title = "Retrieval Guide",
    .content = "RAG retrieval pipeline builder.",
    .source_uri = "docs/rag.md",
  });

  knowledge_query query;
  query.text = "RAG retrieval";
  query.limit = 1;
  const auto results = pipeline.retriever().retrieve(query);
  require(results.size() == 1, "knowledge pipeline should retrieve ingested content");

  const auto block = pipeline.context().build_context_block("RAG retrieval");
  require(contains(block, "docs/rag.md"),
    "knowledge pipeline should create context with configured retriever");
}

void test_knowledge_pipeline_builder_requires_embedding_model() {
  bool threw = false;
  try {
    auto pipeline = knowledge_pipeline::make().local().build();
    (void)pipeline;
  }
  catch (const std::invalid_argument&) {
    threw = true;
  }
  require(threw, "knowledge pipeline builder should require embedding model");
}

void test_knowledge_pipeline_builds_from_json_config() {
  const nlohmann::json config {
    { "backend", "local" },
    { "chunking", {
      { "max_chars", 80 },
      { "overlap_chars", 0 },
    } },
    { "context", {
      { "max_results", 1 },
      { "max_context_chars", 500 },
    } },
    { "cache", {
      { "enabled", true },
      { "max_entries", 8 },
      { "ttl_ms", 1000 },
    } },
    { "reranker", {
      { "type", "bm25" },
    } },
  };

  auto pipeline = build_knowledge_pipeline_from_json(
    config,
    std::make_shared<topic_embedding_model>());

  pipeline.retriever().ingest({
    .id = "configured-doc",
    .content = "RAG retrieval configured pipeline.",
    .source_uri = "docs/configured.md",
  });

  knowledge_query query;
  query.text = "RAG retrieval";
  query.limit = 1;
  const auto first = pipeline.retriever().retrieve_detailed(query);
  const auto second = pipeline.retriever().retrieve_detailed(query);
  require(first.results.size() == 1, "configured pipeline should retrieve content");
  require(second.trace.cache_hit, "configured pipeline should enable retrieval cache");

  const auto block = pipeline.context().build_context_block("RAG retrieval");
  require(contains(block, "docs/configured.md"),
    "configured pipeline should apply context policy");
}

void test_knowledge_pipeline_config_rejects_unknown_backend() {
  bool threw = false;
  try {
    auto pipeline = build_knowledge_pipeline_from_json(
      nlohmann::json { { "backend", "unknown" } },
      std::make_shared<topic_embedding_model>());
    (void)pipeline;
  }
  catch (const std::invalid_argument&) {
    threw = true;
  }
  require(threw, "pipeline config should reject unknown backend");
}

void test_grounding_checker_validates_citations() {
  knowledge_grounding_checker checker;
  std::vector<knowledge_result> results {
    { .chunk = { .id = "chunk-1", .document_id = "doc" }, .score = 1.0 },
  };

  const auto grounded = checker.check("RAG uses retrieved context [1].", results);
  require(grounded.grounded, "grounding checker should accept valid citations");
  require(grounded.citation_numbers.size() == 1 && grounded.citation_numbers.front() == 1,
    "grounding checker should extract citation numbers");

  const auto invalid = checker.check("RAG uses retrieved context [2].", results);
  require(!invalid.grounded, "grounding checker should reject out-of-range citations");
  require(invalid.invalid_citation_numbers.size() == 1,
    "grounding checker should report invalid citations");

  knowledge_grounding_checker strict({
    .require_at_least_one_citation = true,
    .require_sentence_citations = true,
  });
  const auto unsupported = strict.check("Supported claim [1]. Unsupported claim.", results);
  require(!unsupported.grounded,
    "grounding checker should reject uncited sentences in strict mode");
  require(unsupported.unsupported_sentences.size() == 1,
    "grounding checker should report unsupported sentences");
}

void run(const char* name, void (*test)()) {
  test();
  println("[PASS] {}", name);
}

} // namespace

int main() {
  try {
    run("splitter chunks with overlap", test_splitter_chunks_with_overlap);
    run("splitter respects markdown headings", test_splitter_respects_markdown_headings);
    run("splitter skips markdown headings for tika documents",
      test_splitter_skips_markdown_headings_for_tika_documents);
    run("splitter adds pdf page and section metadata",
      test_splitter_adds_pdf_page_and_section_metadata);
    run("splitter avoids pdf bibliography items as sections",
      test_splitter_avoids_pdf_bibliography_items_as_sections);
    run("splitter can add document summary chunk",
      test_splitter_can_add_document_summary_chunk);
    run("splitter summary chunk uses llm summary and toc",
      test_splitter_summary_chunk_uses_llm_summary_and_toc);
    run("path to utf8 preserves unicode on windows",
      test_path_to_utf8_preserves_unicode_on_windows);
    run("splitter prefers paragraph boundaries", test_splitter_prefers_paragraph_boundaries);
    run("splitter supports token windows", test_splitter_supports_token_windows);
    run("splitter avoids splitting markdown code fences",
      test_splitter_avoids_splitting_markdown_code_fences);
    run("splitter respects code symbols", test_splitter_respects_code_symbols);
    run("file loader loads document", test_file_loader_loads_document);
    run("file loader extracts html text", test_file_loader_extracts_html_text);
    run("file loader extracts rtf text", test_file_loader_extracts_rtf_text);
    run("tika loader extracts remote parser text",
      test_tika_loader_extracts_remote_parser_text);
    run("tika loader extracts pdf page breaks from html",
      test_tika_loader_extracts_pdf_page_breaks_from_html);
    run("tika loader live integration when configured",
      test_tika_loader_live_integration_when_configured);
    run("parser registry selects file and tika parsers",
      test_parser_registry_selects_file_and_tika_parsers);
    run("document loader loads files with stable metadata",
      test_document_loader_loads_files_with_stable_metadata);
    run("document loader runs enrichers",
      test_document_loader_runs_enrichers);
    run("structured loader loads csv rows", test_structured_loader_loads_csv_rows);
    run("structured loader flattens json paths", test_structured_loader_flattens_json_paths);
    run("structured loader summarizes openapi json",
      test_structured_loader_summarizes_openapi_json);
    run("directory loader loads supported files", test_directory_loader_loads_supported_files);
    run("code loader loads repository sources", test_code_loader_loads_repository_sources);
    run("incremental ingest skips updates and erases stale",
      test_incremental_ingest_skips_updates_and_erases_stale);
    run("file store reload and rebuild index", test_file_store_reload_and_rebuild_index);
    run("file index reload without rebuild", test_file_index_reload_without_rebuild);
    run("sqlite index reload without rebuild", test_sqlite_index_reload_without_rebuild);
    run("qdrant knowledge upsert payload includes chunk metadata",
      test_qdrant_knowledge_upsert_payload_includes_chunk_metadata);
    run("qdrant knowledge search builds filters and parses results",
      test_qdrant_knowledge_search_builds_filters_and_parses_results);
    run("qdrant knowledge live integration when configured",
      test_qdrant_knowledge_live_integration_when_configured);
    run("remote vector indexes send standard requests",
      test_remote_vector_indexes_send_standard_requests);
    run("file store erase persists", test_file_store_erase_persists);
    run("rebuild index detailed reports partial failures",
      test_rebuild_index_detailed_reports_partial_failures);
    run("rebuild index detailed async reports progress",
      test_rebuild_index_detailed_async_reports_progress);
    run("rebuild index detailed async retries transient failures",
      test_rebuild_index_detailed_async_retries_transient_failures);
    run("ingest batch reports partial failures", test_ingest_batch_reports_partial_failures);
    run("ingest batch async reports progress", test_ingest_batch_async_reports_progress);
    run("ingest batch async can be canceled", test_ingest_batch_async_can_be_canceled);
    run("ingest batch async retries transient failures",
      test_ingest_batch_async_retries_transient_failures);
    run("indexing policy records embedding metadata and dimension",
      test_indexing_policy_records_embedding_metadata_and_dimension);
    run("retriever returns relevant chunk", test_retriever_returns_relevant_chunk);
    run("retriever applies access scope", test_retriever_applies_access_scope);
    run("bm25 reranker promotes exact lexical match", test_bm25_reranker_promotes_exact_lexical_match);
    run("query rewriter enables multi query retrieval",
      test_query_rewriter_enables_multi_query_retrieval);
    run("candidate limit applies without reranker",
      test_candidate_limit_applies_without_reranker);
    run("http query rewriter calls remote service",
      test_http_query_rewriter_calls_remote_service);
    run("llm query rewriter parses json array response",
      test_llm_query_rewriter_parses_json_array_response);
    run("mmr reranker promotes diverse results", test_mmr_reranker_promotes_diverse_results);
    run("cross encoder reranker uses model scores",
      test_cross_encoder_reranker_uses_model_scores);
    run("http cross encoder scorer calls remote service",
      test_http_cross_encoder_scorer_calls_remote_service);
    run("retrieve detailed reports trace metrics", test_retrieve_detailed_reports_trace_metrics);
    run("retriever publishes observability events",
      test_retriever_publishes_observability_events);
    run("retrieval cache hits and invalidates", test_retrieval_cache_hits_and_invalidates);
    run("retrieval cache eviction and ttl", test_retrieval_cache_eviction_and_ttl);
    run("cached reranker reuses results", test_cached_reranker_reuses_results);
    run("cached reranker eviction and ttl", test_cached_reranker_eviction_and_ttl);
    run("knowledge metrics export prometheus and otel",
      test_knowledge_metrics_export_prometheus_and_otel);
    run("context augments request with citations", test_context_augments_request_with_citations);
    run("result processor dedupes and merges adjacent chunks",
      test_result_processor_dedupes_and_merges_adjacent_chunks);
    run("context merges adjacent chunks before injection",
      test_context_merges_adjacent_chunks_before_injection);
    run("context expands retrieved chunks with neighbors",
      test_context_expands_retrieved_chunks_with_neighbors);
    run("context truncates oversized chunks", test_context_truncates_oversized_chunks);
    run("hybrid retrieval uses lexical score and threshold",
      test_hybrid_retrieval_uses_lexical_score_and_threshold);
    run("erase document removes results", test_erase_document_removes_results);
    run("knowledge tool provider searches with citations",
      test_knowledge_tool_provider_searches_with_citations);
    run("evaluate knowledge retrieval reports recall and mrr",
      test_evaluate_knowledge_retrieval_reports_recall_and_mrr);
    run("knowledge eval loads cases and reports terms",
      test_knowledge_eval_loads_cases_and_reports_terms);
    run("knowledge eval runs offline fixture",
      test_knowledge_eval_runs_offline_fixture);
    run("rag service uploads document and answers",
      test_rag_service_uploads_document_and_answers);
    run("knowledge benchmark reports latency", test_knowledge_benchmark_reports_latency);
    run("knowledge benchmark loads query file", test_knowledge_benchmark_loads_query_file);
    run("knowledge store migration audits and reindexes target",
      test_knowledge_store_migration_audits_and_reindexes_target);
    run("knowledge pipeline builder local preset", test_knowledge_pipeline_builder_local_preset);
    run("knowledge pipeline builder requires embedding model",
      test_knowledge_pipeline_builder_requires_embedding_model);
    run("knowledge pipeline builds from json config",
      test_knowledge_pipeline_builds_from_json_config);
    run("knowledge pipeline config rejects unknown backend",
      test_knowledge_pipeline_config_rejects_unknown_backend);
    run("grounding checker validates citations", test_grounding_checker_validates_citations);
  }
  catch (const std::exception& ex) {
    println("[FAIL] {}", ex.what());
    return 1;
  }

  return 0;
}
