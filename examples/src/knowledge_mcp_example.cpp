#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <wuwe/agent/knowledge/file_knowledge_index.hpp>
#include <wuwe/agent/knowledge/file_knowledge_loader.hpp>
#include <wuwe/agent/knowledge/file_knowledge_store.hpp>
#include <wuwe/agent/knowledge/knowledge_splitter.hpp>
#include <wuwe/agent/knowledge/knowledge_tools.hpp>
#include <wuwe/agent/mcp/mcp_server.hpp>
#include <wuwe/agent/mcp/mcp_stdio_transport.hpp>
#include <wuwe/agent/memory/embedding_model.hpp>

class example_embedding_model final : public wuwe::agent::memory::embedding_model {
public:
  std::vector<float> embed(std::string_view text) const override {
    const std::string value(text);
    if (value.find("RAG") != std::string::npos ||
        value.find("retrieval") != std::string::npos ||
        value.find("citation") != std::string::npos) {
      return { 1.0F, 0.0F, 0.0F };
    }
    return { 0.0F, 1.0F, 0.0F };
  }
};

std::filesystem::path unique_temp_path(std::string_view prefix, std::string_view suffix) {
  const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch())
                       .count();
  return std::filesystem::temp_directory_path() /
         (std::string(prefix) + std::to_string(stamp) + std::string(suffix));
}

int main() {
  namespace knowledge = wuwe::agent::knowledge;

  const auto document_path = unique_temp_path("wuwe-knowledge-mcp-", ".md");
  const auto store_path = unique_temp_path("wuwe-knowledge-mcp-store-", ".jsonl");
  const auto index_path = unique_temp_path("wuwe-knowledge-mcp-index-", ".jsonl");

  {
    std::ofstream document(document_path, std::ios::binary);
    document << "# Wuwe RAG MCP\n"
                "RAG retrieves relevant document chunks and returns citations with source URIs.\n"
                "MCP hosts can call search_knowledge to inspect grounded context before answering.\n";
  }

  auto retriever = std::make_shared<knowledge::knowledge_retriever>(
    std::make_shared<knowledge::file_knowledge_store>(store_path),
    std::make_shared<knowledge::file_knowledge_index>(index_path),
    std::make_shared<example_embedding_model>(),
    knowledge::knowledge_splitter({
      .max_chars = 500,
      .overlap_chars = 40,
    }));

  knowledge::file_knowledge_loader loader;
  retriever->ingest(loader.load(document_path, {
    .id = "wuwe-rag-mcp",
    .title = "Wuwe RAG MCP",
    .source_uri = "wuwe://examples/knowledge-mcp",
    .metadata = { { "topic", "rag" } },
  }));

  knowledge::knowledge_tool_provider tools(*retriever, {
    .max_search_results = 5,
    .minimum_score = 0.0,
  });

  wuwe::agent::mcp::mcp_server server({ .name = "wuwe-knowledge-mcp", .version = "0.1.0" });
  server.add_tool_provider(tools);
  server.add_resource(
    {
      .uri = "wuwe://examples/knowledge-mcp",
      .name = "Wuwe RAG MCP Example",
      .description = "Seed document used by the search_knowledge MCP tool.",
      .mime_type = "text/markdown",
    },
    [] {
      return std::vector<wuwe::agent::mcp::mcp_resource_content> {
        wuwe::agent::mcp::mcp_resource_content::text_content(
          "wuwe://examples/knowledge-mcp",
          "# Wuwe RAG MCP\n"
          "RAG retrieves relevant document chunks and returns citations with source URIs.\n"
          "MCP hosts can call search_knowledge to inspect grounded context before answering.\n",
          "text/markdown"),
      };
    });
  server.set_access_policy({
    .allowed_tools = { "search_knowledge" },
    .allowed_resources = { "wuwe://examples/knowledge-mcp" },
  });

  wuwe::agent::mcp::mcp_stdio_transport transport;
  return transport.run_stdio(server);
}
