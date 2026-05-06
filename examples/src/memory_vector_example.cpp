#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <wuwe/agent/memory/embedding_model.hpp>
#include <wuwe/agent/memory/hybrid_memory_ranker.hpp>
#include <wuwe/agent/memory/in_memory_store.hpp>
#include <wuwe/agent/memory/in_memory_vector_index.hpp>
#include <wuwe/agent/memory/memory_context.hpp>
#include <wuwe/agent/memory/qdrant_memory_index.hpp>
#include <wuwe/common/print.h>

namespace {

std::string env_value(const char* name) {
#if defined(_MSC_VER)
  char* value = nullptr;
  std::size_t length = 0;
  if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) {
    return {};
  }
  std::string result(value);
  free(value);
  return result;
#else
  const auto* value = std::getenv(name);
  return value ? std::string(value) : std::string {};
#endif
}

class deterministic_embedding_model final : public wuwe::agent::memory::embedding_model {
public:
  std::vector<float> embed(std::string_view text) const override {
    if (text.find("ownership") != std::string_view::npos ||
        text.find("API") != std::string_view::npos ||
        text.find("interface") != std::string_view::npos) {
      return { 1.0F, 0.0F, 0.0F };
    }
    if (text.find("notebook") != std::string_view::npos ||
        text.find("analysis") != std::string_view::npos) {
      return { 0.0F, 1.0F, 0.0F };
    }
    return { 0.0F, 0.0F, 1.0F };
  }

  std::vector<std::vector<float>> embed_batch(
    const std::vector<std::string>& texts) const override {
    std::vector<std::vector<float>> result;
    result.reserve(texts.size());
    for (const auto& text : texts) {
      result.push_back(embed(text));
    }
    return result;
  }
};

} // namespace

int main() {
  namespace memory = wuwe::agent::memory;

  auto short_term = std::make_shared<memory::in_memory_store>();
  auto long_term = std::make_shared<memory::in_memory_store>();

  memory::memory_policy policy;
  policy.max_long_term_records = 4;
  policy.vector_rebuild_batch_size = 2;

  memory::memory_context context(short_term, long_term, policy);
  context.set_scope({
    .user_id = "local-user",
    .application_id = "wuwe-vector-example",
    .conversation_id = "demo-session",
    .agent_id = "assistant",
  });
  context.set_embedding_model(std::make_shared<deterministic_embedding_model>());
  context.set_ranker(std::make_shared<memory::hybrid_memory_ranker>());

  const auto qdrant_url = env_value("WUWE_QDRANT_URL");
  if (!qdrant_url.empty()) {
    context.set_vector_index(std::make_shared<memory::qdrant_memory_index>(
      memory::qdrant_memory_index_config {
        .base_url = qdrant_url,
        .collection_name = "wuwe_memory_example",
        .embedding_provider = "deterministic-example",
        .embedding_model = "deterministic-3d",
        .embedding_version = "1",
      }));
    wuwe::println("Using Qdrant vector index at {}", qdrant_url);
  }
  else {
    context.set_vector_index(std::make_shared<memory::in_memory_vector_index>());
    wuwe::println("Using in-memory vector index. Set WUWE_QDRANT_URL to use Qdrant.");
  }

  context.remember_long_term(
    "Use explicit ownership in public APIs.",
    context.scope(),
    { { "topic", "api-style" } });
  context.remember_long_term(
    "Prefer notebooks for exploratory analysis.",
    context.scope(),
    { { "topic", "analysis" } });

  const auto rebuild = context.rebuild_vector_index_detailed();
  wuwe::println(
    "Rebuild scanned={} rebuilt={} errors={}",
    rebuild.scanned,
    rebuild.rebuilt,
    rebuild.errors.size());

  memory::memory_query query;
  query.scope = context.scope();
  query.kinds = { memory::memory_kind::long_term };
  query.text = "Design an interface for the memory API.";
  query.limit = 2;

  for (const auto& record : context.recall(query)) {
    wuwe::println("- [{}] {}", memory::to_string(record.kind), record.content);
  }
}
