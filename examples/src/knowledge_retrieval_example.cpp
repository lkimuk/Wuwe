#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <wuwe/agent/knowledge/file_knowledge_loader.hpp>
#include <wuwe/agent/knowledge/file_knowledge_index.hpp>
#include <wuwe/agent/knowledge/file_knowledge_store.hpp>
#include <wuwe/agent/knowledge/knowledge_context.hpp>
#include <wuwe/agent/knowledge/knowledge_tools.hpp>
#include <wuwe/agent/memory/embedding_model.hpp>
#include <wuwe/wuwe.h>

class simple_topic_embedding_model final : public wuwe::agent::memory::embedding_model {
public:
  std::vector<float> embed(std::string_view text) const override {
    const std::string value(text);
    if (value.find("retrieval") != std::string::npos ||
        value.find("Retrieval") != std::string::npos ||
        value.find("RAG") != std::string::npos ||
        value.find("cite") != std::string::npos ||
        value.find("Citations") != std::string::npos) {
      return { 1.0F, 0.0F, 0.0F };
    }
    return { 0.0F, 1.0F, 0.0F };
  }
};

std::filesystem::path write_example_markdown() {
  const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch())
                       .count();
  const auto path = std::filesystem::temp_directory_path() /
                    ("wuwe-knowledge-retrieval-" + std::to_string(stamp) + ".md");

  std::ofstream output(path, std::ios::binary);
  output << "# Knowledge Retrieval\n"
            "Knowledge retrieval, also called RAG, searches external documents and injects "
            "the most relevant cited chunks into the model request.\n\n"
            "## Citations\n"
            "Each retrieved chunk should preserve a source URI so the generated answer can "
            "point back to the supporting material.\n";
  return path;
}

std::filesystem::path example_store_path() {
  const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch())
                       .count();
  return std::filesystem::temp_directory_path() /
         ("wuwe-knowledge-store-" + std::to_string(stamp) + ".jsonl");
}

std::filesystem::path example_index_path() {
  const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch())
                       .count();
  return std::filesystem::temp_directory_path() /
         ("wuwe-knowledge-index-" + std::to_string(stamp) + ".jsonl");
}

int main() {
  namespace knowledge = wuwe::agent::knowledge;

  const auto markdown_path = write_example_markdown();
  const auto store_path = example_store_path();
  const auto index_path = example_index_path();

  {
    auto retriever = std::make_shared<knowledge::knowledge_retriever>(
      std::make_shared<knowledge::file_knowledge_store>(store_path),
      std::make_shared<knowledge::file_knowledge_index>(index_path),
      std::make_shared<simple_topic_embedding_model>(),
      knowledge::knowledge_splitter({
        .max_chars = 400,
        .overlap_chars = 40,
      }));

    knowledge::file_knowledge_loader loader;
    retriever->ingest(loader.load(markdown_path, {
      .id = "knowledge-retrieval-guide",
      .title = "Knowledge Retrieval Guide",
      .source_uri = "docs/knowledge-retrieval.md",
      .metadata = { { "topic", "rag" } },
    }));
  }

  auto retriever = std::make_shared<knowledge::knowledge_retriever>(
    std::make_shared<knowledge::file_knowledge_store>(store_path),
    std::make_shared<knowledge::file_knowledge_index>(index_path),
    std::make_shared<simple_topic_embedding_model>());

  knowledge::knowledge_context context(retriever);
  knowledge::knowledge_tool_provider tools(*retriever);

  auto request = wuwe::make_message()
                 << ("system" < wuwe::says > "Answer using the provided knowledge.")
                 << ("user" < wuwe::says > "How should a RAG answer cite retrieved material?");

  const auto augmented = context.augment(std::move(request), "RAG retrieval citations");

  wuwe::println("Knowledge retrieval example");
  wuwe::println("Retrieved context is injected as a separate knowledge block.\n");

  for (const auto& message : augmented.messages) {
    wuwe::println("[{}] {}\n", message.role, message.content);
  }

  const auto tool_result = tools.invoke(
    "search_knowledge",
    R"({"content":"RAG retrieval citations","limit":1})");
  wuwe::println("Tool search result:\n{}\n", tool_result.content);

  std::error_code ignored;
  std::filesystem::remove(markdown_path, ignored);
  std::filesystem::remove(store_path, ignored);
  std::filesystem::remove(store_path.string() + ".tmp", ignored);
  std::filesystem::remove(index_path, ignored);
  std::filesystem::remove(index_path.string() + ".tmp", ignored);
}
