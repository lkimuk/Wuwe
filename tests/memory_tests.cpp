#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <wuwe/agent/llm/llm_agent_runner.h>
#include <wuwe/agent/memory/file_memory_store.hpp>
#include <wuwe/agent/memory/embedding_model.hpp>
#include <wuwe/agent/memory/hybrid_memory_ranker.hpp>
#include <wuwe/agent/memory/in_memory_store.hpp>
#include <wuwe/agent/memory/in_memory_vector_index.hpp>
#include <wuwe/agent/memory/memory_context.hpp>
#include <wuwe/agent/memory/memory_tools.hpp>
#include <wuwe/agent/memory/openai_embedding_model.hpp>
#include <wuwe/agent/memory/qdrant_memory_index.hpp>
#include <wuwe/agent/memory/sqlite_memory_store.hpp>
#include <wuwe/common/print.h>
#include <wuwe/net/http_client.h>

namespace {

using namespace wuwe;
using namespace wuwe::agent::memory;

void require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

memory_scope test_scope(std::string conversation_id = "conversation-a") {
  return {
    .tenant_id = "tenant-a",
    .user_id = "user-a",
    .application_id = "memory-tests",
    .conversation_id = std::move(conversation_id),
    .agent_id = "agent-a",
  };
}

bool contains(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

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

class preferred_ranker final : public memory_ranker {
public:
  std::vector<memory_record> rank(
    const memory_query& query,
    std::vector<memory_record> candidates) const override {
    std::sort(candidates.begin(), candidates.end(), [](const memory_record& lhs,
                                                        const memory_record& rhs) {
      return lhs.content > rhs.content;
    });

    if (candidates.size() > query.limit) {
      candidates.resize(query.limit);
    }
    return candidates;
  }
};

class topic_embedding_model final : public embedding_model {
public:
  std::vector<float> embed(std::string_view text) const override {
    const auto contains = [&](std::string_view needle) {
      return text.find(needle) != std::string_view::npos;
    };

    if (contains("ownership") || contains("template") || contains("API")) {
      return { 1.0F, 0.0F, 0.0F };
    }
    if (contains("Python") || contains("notebook")) {
      return { 0.0F, 1.0F, 0.0F };
    }
    return { 0.0F, 0.0F, 1.0F };
  }
};

class failing_embedding_model final : public embedding_model {
public:
  std::vector<float> embed(std::string_view) const override {
    throw std::runtime_error("embedding failed");
  }
};

class counting_batch_embedding_model final : public embedding_model {
public:
  std::vector<float> embed(std::string_view) const override {
    ++single_calls;
    return { 1.0F, 0.0F, 0.0F };
  }

  std::vector<std::vector<float>> embed_batch(
    const std::vector<std::string>& texts) const override {
    ++batch_calls;
    batch_sizes.push_back(texts.size());
    std::vector<std::vector<float>> result;
    result.reserve(texts.size());
    for (std::size_t index = 0; index < texts.size(); ++index) {
      result.push_back({ 1.0F, 0.0F, 0.0F });
    }
    return result;
  }

  mutable int single_calls { 0 };
  mutable int batch_calls { 0 };
  mutable std::vector<std::size_t> batch_sizes;
};

class counting_vector_index final : public vector_memory_index {
public:
  void upsert(const memory_record& record, const std::vector<float>& embedding) override {
    ++single_upserts;
    records.push_back(record);
    embeddings.push_back(embedding);
  }

  void upsert_batch(
    const std::vector<memory_record>& batch_records,
    const std::vector<std::vector<float>>& batch_embeddings) override {
    ++batch_upserts;
    batch_sizes.push_back(batch_records.size());
    for (std::size_t index = 0; index < batch_records.size(); ++index) {
      records.push_back(batch_records[index]);
      embeddings.push_back(batch_embeddings[index]);
    }
  }

  std::vector<vector_memory_match> search(const vector_memory_query&) const override {
    return {};
  }

  bool erase(const std::string&, const memory_scope&) override {
    return false;
  }

  std::size_t clear(const memory_scope&) override {
    ++clears;
    records.clear();
    embeddings.clear();
    return 0;
  }

  int single_upserts { 0 };
  int batch_upserts { 0 };
  int clears { 0 };
  std::vector<std::size_t> batch_sizes;
  std::vector<memory_record> records;
  std::vector<std::vector<float>> embeddings;
};

class capture_client final : public llm_client {
public:
  llm_response complete(const llm_request& request) override {
    last_request = request;
    return {
      .content = "assistant response",
    };
  }

  llm_request last_request;
};

class tool_call_client final : public llm_client {
public:
  llm_response complete(const llm_request& request) override {
    ++calls;
    requests.push_back(request);

    if (calls == 1) {
      return {
        .content = "",
        .tool_calls = {
          {
            .id = "call-1",
            .name = "save_memory",
            .arguments_json =
              R"({"content":"User prefers terse C++20 examples.","topic":"preference"})",
          },
        },
      };
    }

    return {
      .content = "done",
    };
  }

  int calls { 0 };
  std::vector<llm_request> requests;
};

class embedding_http_client final : public http_client {
public:
  http_response send(const http_request& request) override {
    requests.push_back(request);
    return {
      .body =
        R"({"object":"list","data":[{"object":"embedding","index":0,"embedding":[1.0,0.0,0.0]}],"model":"embedding-test"})",
    };
  }

  std::vector<http_request> requests;
};

class batch_embedding_http_client final : public http_client {
public:
  http_response send(const http_request& request) override {
    requests.push_back(request);
    return {
      .body =
        R"({"object":"list","data":[{"object":"embedding","index":1,"embedding":[0.0,1.0]},{"object":"embedding","index":0,"embedding":[1.0,0.0]}],"model":"embedding-test"})",
    };
  }

  std::vector<http_request> requests;
};

class qdrant_capture_http_client final : public http_client {
public:
  http_response send(const http_request& request) override {
    requests.push_back(request);
    return {
      .body = R"({"result":{"status":"ok"}})",
    };
  }

  std::vector<http_request> requests;
};

void test_scoped_recall_and_isolation() {
  memory_context memory;
  memory.set_scope(test_scope("conversation-a"));

  memory.remember_working("alpha scoped memory");

  memory_query empty_scope_query;
  empty_scope_query.text = "alpha";
  empty_scope_query.kinds = { memory_kind::working };
  require(memory.recall(empty_scope_query).empty(), "empty scope should not recall records");

  memory_query matching_query;
  matching_query.text = "alpha";
  matching_query.scope = test_scope("conversation-a");
  matching_query.kinds = { memory_kind::working };
  require(memory.recall(matching_query).size() == 1, "matching scope should recall record");

  memory_query other_conversation_query = matching_query;
  other_conversation_query.scope = test_scope("conversation-b");
  require(memory.recall(other_conversation_query).empty(), "different conversation should not recall record");
}

void test_long_term_scope_required() {
  memory_context memory;

  bool threw = false;
  try {
    memory.remember_long_term("durable fact", {});
  }
  catch (const std::invalid_argument&) {
    threw = true;
  }

  require(threw, "long-term memory without persistent scope should throw");

  const auto saved = memory.remember_long_term("durable fact", test_scope());
  require(!saved.id.empty(), "long-term memory with persistent scope should be saved");
}

void test_hidden_secret_and_request_dedupe() {
  memory_context memory;
  memory.set_scope(test_scope());

  memory.remember_working("visible working item");

  memory_record hidden;
  hidden.kind = memory_kind::working;
  hidden.visibility = memory_visibility::hidden;
  hidden.content = "hidden working item";
  hidden.scope = test_scope();
  memory.remember(hidden);

  memory.remember_working("secret working item", { { "sensitivity", "secret" } });
  memory.observe({ .role = "user", .content = "current request should not repeat" });

  llm_request request;
  request.messages.push_back({ .role = "user", .content = "current request should not repeat" });
  const auto augmented = memory.augment(std::move(request), "working current request");

  require(augmented.messages.size() == 2, "visible memory should inject one memory message");
  const auto& block = augmented.messages.front().content;
  require(contains(block, "visible working item"), "visible memory should be injected");
  require(!contains(block, "hidden working item"), "hidden memory should not be injected");
  require(!contains(block, "secret working item"), "secret memory should not be injected");
  require(!contains(block, "current request should not repeat"), "current request should be deduped");
}

void test_memory_inspection_list_get_and_filters() {
  memory_context memory;
  memory.set_scope(test_scope());

  memory.remember_working("temporary implementation note", { { "topic", "work" } });
  const auto api_memory = memory.remember_long_term(
    "Use explicit ownership in public APIs.",
    test_scope(),
    { { "topic", "api-style" }, { "sensitivity", "internal" } });
  memory.remember_long_term(
    "Prefer Python notebooks for exploratory analysis.",
    test_scope(),
    { { "topic", "analysis" } });
  memory.remember_long_term(
    "Other conversation memory",
    test_scope("conversation-b"),
    { { "topic", "api-style" } });

  memory_query long_term_query;
  long_term_query.kinds = { memory_kind::long_term };
  long_term_query.filters = { { "topic", "api-style" } };
  long_term_query.limit = 10;

  const auto listed = memory.list(long_term_query);
  require(listed.size() == 1, "inspection list should use active scope and metadata filters");
  require(listed.front().id == api_memory.id, "inspection list should return matching memory");

  const auto loaded = memory.get(api_memory.id, test_scope());
  require(loaded.has_value(), "inspection get should load memory by id and scope");
  require(loaded->metadata.at("sensitivity") == "internal",
    "inspection get should preserve metadata");

  memory_query sensitivity_query;
  sensitivity_query.scope = test_scope();
  sensitivity_query.kinds = { memory_kind::long_term };
  sensitivity_query.filters = { { "sensitivity", "internal" } };
  sensitivity_query.limit = 10;
  require(memory.list(sensitivity_query).size() == 1,
    "inspection list should filter by sensitivity metadata");

  memory_query empty_scope_query;
  empty_scope_query.scope = {};
  empty_scope_query.kinds = { memory_kind::long_term };
  empty_scope_query.limit = 10;
  memory.set_scope({});
  require(memory.list(empty_scope_query).empty(),
    "inspection list should honor scoped recall policy for empty scope");
}

void test_memory_inspection_delete_and_clear() {
  memory_context memory;
  memory.set_scope(test_scope());

  const auto first = memory.remember_long_term("first durable memory", test_scope());
  memory.remember_long_term("second durable memory", test_scope());
  memory.remember_working("working memory");

  require(memory.erase(first.id, test_scope()), "inspection erase should delete memory");
  require(!memory.get(first.id, test_scope()).has_value(),
    "inspection get should not return erased memory");

  memory_query query;
  query.scope = test_scope();
  query.limit = 10;
  require(memory.list(query).size() == 2,
    "inspection list should include remaining long-term and working memory");

  require(memory.clear(test_scope()) == 2,
    "inspection clear should remove remaining scoped memory");
  require(memory.list(query).empty(), "inspection list should be empty after clear");
}

void test_conversation_summarization_callback() {
  memory_context memory;
  memory.set_scope(test_scope());
  memory.observe({ .role = "user", .content = "We need vector memory." });
  memory.observe({ .role = "assistant", .content = "Use Qdrant as the vector index." });

  const auto result = memory.summarize_conversation(
    {
      .scope = test_scope(),
      .metadata = { { "topic", "design" } },
    },
    [](const std::vector<memory_record>& records) {
      require(records.size() == 2, "summarizer callback should receive source records");
      return records.front().content + " / " + records.back().content;
    });

  require(result.summary.has_value(), "conversation summarization should create a summary");
  require(result.source_count == 2, "conversation summarization should report source count");
  require(result.summary->kind == memory_kind::summary,
    "conversation summarization should save summary kind");
  require(result.summary->metadata.at("source") == "conversation_summary",
    "conversation summarization should mark source metadata");
  require(result.summary->metadata.at("topic") == "design",
    "conversation summarization should preserve custom metadata");
}

void test_conversation_summarization_can_erase_sources() {
  memory_context memory;
  memory.set_scope(test_scope());
  memory.observe({ .role = "user", .content = "Old message one." });
  memory.observe({ .role = "assistant", .content = "Old message two." });

  const auto result = memory.summarize_conversation(
    {
      .scope = test_scope(),
      .erase_source_records = true,
    },
    [](const std::vector<memory_record>&) {
      return "Summarized old messages.";
    });
  require(result.summary.has_value(), "conversation summarization should create summary");

  memory_query conversation_query;
  conversation_query.scope = test_scope();
  conversation_query.kinds = { memory_kind::conversation };
  conversation_query.limit = 10;
  require(memory.list(conversation_query).empty(),
    "conversation summarization should erase source records when requested");

  memory_query summary_query;
  summary_query.scope = test_scope();
  summary_query.kinds = { memory_kind::summary };
  summary_query.limit = 10;
  require(memory.list(summary_query).size() == 1,
    "conversation summarization should keep summary record");
}

void test_budget_and_ranker() {
  memory_policy policy;
  policy.max_memory_chars = 1000;
  policy.max_memory_tokens = 7;
  policy.estimated_chars_per_token = 4;
  policy.include_conversation = false;
  policy.include_long_term = false;
  policy.include_summaries = false;

  memory_context memory(policy);
  memory.set_scope(test_scope());
  memory.remember_working("zz preferred");
  memory.remember_working("aa not preferred");
  memory.set_ranker(std::make_shared<preferred_ranker>());

  const auto block = memory.build_memory_block("preferred");
  require(contains(block, "zz preferred"), "custom ranker should control selected ordering");
  require(!contains(block, "aa not preferred"), "token estimate budget should cap memory block");
}

void test_file_store_reload_and_ids() {
  const auto path = std::filesystem::temp_directory_path() /
                    ("wuwe-memory-test-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                     ".jsonl");

  const auto cleanup = [&] {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(path.string() + ".tmp", ignored);
  };

  cleanup();

  {
    file_memory_store store(path);
    memory_record first;
    first.kind = memory_kind::long_term;
    first.content = "persisted one";
    first.scope = test_scope();
    const auto saved = store.add(first);
    require(saved.id == "mem-1", "first generated id should be mem-1");
  }

  {
    file_memory_store store(path);
    memory_query query;
    query.scope = test_scope();
    query.kinds = { memory_kind::long_term };
    query.text = "persisted";
    const auto records = store.search(query);
    require(records.size() == 1, "file store should reload persisted record");

    memory_record second;
    second.kind = memory_kind::long_term;
    second.content = "persisted two";
    second.scope = test_scope();
    const auto saved = store.add(second);
    require(saved.id == "mem-2", "file store should continue generated ids after reload");
  }

  cleanup();
}

void test_vector_index_semantic_recall() {
  memory_context memory;
  memory.set_scope(test_scope());
  memory.set_embedding_model(std::make_shared<topic_embedding_model>());
  memory.set_vector_index(std::make_shared<in_memory_vector_index>());

  memory.remember_long_term(
    "Use explicit ownership in public APIs.",
    test_scope(),
    { { "topic", "api-style" } });
  memory.remember_long_term(
    "Prefer Python notebooks for exploratory analysis.",
    test_scope(),
    { { "topic", "analysis" } });

  memory_query query;
  query.scope = test_scope();
  query.kinds = { memory_kind::long_term };
  query.text = "template interface design";
  query.limit = 1;

  const auto records = memory.recall(query);
  require(records.size() == 1, "semantic recall should return one record");
  require(contains(records.front().content, "explicit ownership"),
    "semantic recall should prefer the vector-nearest memory");
}

void test_vector_index_lifecycle_sync() {
  memory_context memory;
  memory.set_scope(test_scope());
  memory.set_embedding_model(std::make_shared<topic_embedding_model>());
  memory.set_vector_index(std::make_shared<in_memory_vector_index>());

  auto saved = memory.remember_long_term(
    "Use explicit ownership in public APIs.",
    test_scope(),
    { { "topic", "api-style" } });

  saved.content = "Prefer Python notebooks for exploratory analysis.";
  saved.metadata = { { "topic", "analysis" } };
  require(memory.update(saved), "memory update should succeed");

  memory_query query;
  query.scope = test_scope();
  query.kinds = { memory_kind::long_term };
  query.text = "notebook exploration";
  query.limit = 1;

  auto records = memory.recall(query);
  require(records.size() == 1, "updated vector memory should be recalled");
  require(contains(records.front().content, "Python notebooks"),
    "updated vector memory should use the new content");

  require(memory.erase(saved.id, test_scope()), "memory erase should remove store and vector index");
  require(memory.recall(query).empty(), "erased memory should not be recalled");
}

void test_vector_index_rebuild() {
  memory_context memory;
  memory.set_scope(test_scope());

  memory.remember_long_term(
    "Use explicit ownership in public APIs.",
    test_scope(),
    { { "topic", "api-style" } });
  memory.remember_long_term(
    "Prefer Python notebooks for exploratory analysis.",
    test_scope(),
    { { "topic", "analysis" } });
  memory.remember_long_term(
    "Do not embed secrets.",
    test_scope(),
    { { "sensitivity", "secret" } });

  memory.set_embedding_model(std::make_shared<topic_embedding_model>());
  memory.set_vector_index(std::make_shared<in_memory_vector_index>());

  const auto rebuild = memory.rebuild_vector_index_detailed();
  require(rebuild.scanned == 3, "vector index rebuild should report scanned records");
  require(rebuild.rebuilt == 2, "vector index rebuild should rebuild visible records");
  require(rebuild.skipped_hidden_or_secret == 1,
    "vector index rebuild should count skipped secret records");
  require(rebuild.errors.empty(), "vector index rebuild should not report errors");
  require(memory.rebuild_vector_index() == 2,
    "legacy vector index rebuild should return rebuilt count");

  memory_query query;
  query.scope = test_scope();
  query.kinds = { memory_kind::long_term };
  query.text = "template interface design";
  query.limit = 1;

  const auto records = memory.recall(query);
  require(records.size() == 1, "rebuilt vector index should support recall");
  require(contains(records.front().content, "explicit ownership"),
    "rebuilt vector index should rank semantic match first");
}

void test_vector_index_rebuild_collects_errors() {
  memory_context memory;
  memory.set_scope(test_scope());

  memory.remember_long_term("Use explicit ownership in public APIs.", test_scope());
  memory.set_embedding_model(std::make_shared<failing_embedding_model>());
  memory.set_vector_index(std::make_shared<in_memory_vector_index>());

  const auto rebuild = memory.rebuild_vector_index_detailed();
  require(rebuild.scanned == 1, "failed rebuild should count scanned records");
  require(rebuild.rebuilt == 0, "failed rebuild should not count rebuilt records");
  require(rebuild.errors.size() == 1, "failed rebuild should collect one error");
  require(contains(rebuild.errors.front().message, "embedding failed"),
    "failed rebuild should preserve error message");

  memory_query query;
  query.scope = test_scope();
  query.kinds = { memory_kind::long_term };
  query.limit = 1;
  const auto records = memory.list(query);
  require(records.size() == 1, "failed rebuild should keep authoritative memory");
  require(records.front().metadata.at("index_status") == "pending_reindex",
    "failed rebuild should mark memory as pending reindex");
}

void test_index_failure_marks_pending_reindex_on_write() {
  memory_context memory;
  memory.set_scope(test_scope());
  memory.set_embedding_model(std::make_shared<failing_embedding_model>());
  memory.set_vector_index(std::make_shared<in_memory_vector_index>());

  const auto saved = memory.remember_long_term(
    "Use explicit ownership in public APIs.",
    test_scope());
  require(saved.metadata.at("index_status") == "pending_reindex",
    "index failure on write should mark returned record pending");
  require(contains(saved.metadata.at("index_error"), "embedding failed"),
    "index failure on write should preserve error");

  const auto loaded = memory.get(saved.id, test_scope());
  require(loaded.has_value(), "index failure on write should keep authoritative memory");
  require(loaded->metadata.at("index_status") == "pending_reindex",
    "index failure on write should persist pending status");
}

void test_reconcile_pending_reindex_marks_indexed() {
  memory_context memory;
  memory.set_scope(test_scope());
  memory.set_embedding_model(std::make_shared<failing_embedding_model>());
  memory.set_vector_index(std::make_shared<in_memory_vector_index>());

  const auto saved = memory.remember_long_term(
    "Use explicit ownership in public APIs.",
    test_scope());
  require(saved.metadata.at("index_status") == "pending_reindex",
    "setup should leave memory pending reindex");

  memory.set_embedding_model(std::make_shared<topic_embedding_model>());
  const auto reconcile = memory.reconcile_pending_reindex();
  require(reconcile.scanned == 1, "reconcile should scan pending records");
  require(reconcile.rebuilt == 1, "reconcile should rebuild pending record");
  require(reconcile.errors.empty(), "reconcile should not report errors after provider recovery");

  const auto loaded = memory.get(saved.id, test_scope());
  require(loaded->metadata.at("index_status") == "indexed",
    "reconcile should mark memory indexed");
  require(loaded->metadata.find("index_error") == loaded->metadata.end(),
    "reconcile should clear stale index error");
}

void test_compact_expired_memories() {
  memory_context memory;
  memory.set_scope(test_scope());

  memory_record expired_long;
  expired_long.kind = memory_kind::long_term;
  expired_long.content = "expired long-term memory";
  expired_long.scope = test_scope();
  expired_long.expires_at = std::chrono::system_clock::now() - std::chrono::seconds(1);
  memory.remember(expired_long);

  memory_record retained_long;
  retained_long.kind = memory_kind::long_term;
  retained_long.content = "retained long-term memory";
  retained_long.scope = test_scope();
  retained_long.expires_at = std::chrono::system_clock::now() + std::chrono::hours(1);
  memory.remember(retained_long);

  memory_record expired_working;
  expired_working.kind = memory_kind::working;
  expired_working.content = "expired working memory";
  expired_working.scope = test_scope();
  expired_working.expires_at = std::chrono::system_clock::now() - std::chrono::seconds(1);
  memory.remember(expired_working);

  memory_query query;
  query.scope = test_scope();
  query.limit = 10;
  const auto compacted = memory.compact_expired(query);
  require(compacted.scanned == 3, "compaction should scan all scoped records");
  require(compacted.erased_expired == 2, "compaction should erase expired records");
  require(compacted.errors.empty(), "compaction should not report errors");

  const auto remaining = memory.list(query);
  require(remaining.size() == 1, "compaction should retain non-expired memory");
  require(contains(remaining.front().content, "retained"),
    "compaction should keep the unexpired record");
}

void test_audit_sink_records_memory_operations() {
  memory_context memory;
  memory.set_scope(test_scope());

  std::vector<memory_audit_event> events;
  memory.set_audit_sink([&](const memory_audit_event& event) {
    events.push_back(event);
  });

  const auto saved = memory.remember_long_term("audited memory", test_scope());
  auto updated = saved;
  updated.content = "updated audited memory";
  require(memory.update(updated), "audited update should succeed");
  require(memory.erase(saved.id, test_scope()), "audited erase should succeed");

  require(events.size() >= 3, "audit sink should receive remember/update/erase events");
  require(events[0].action == memory_audit_action::remember && events[0].success,
    "audit sink should record remember success");
  require(events[1].action == memory_audit_action::update && events[1].success,
    "audit sink should record update success");
  require(events[2].action == memory_audit_action::erase && events[2].success,
    "audit sink should record erase success");
}

void test_privacy_filter_rejects_sensitive_memory() {
  memory_context memory;
  memory.set_scope(test_scope());

  std::vector<memory_audit_event> events;
  memory.set_audit_sink([&](const memory_audit_event& event) {
    events.push_back(event);
  });
  memory.set_privacy_filter([](memory_record& record, std::string& reason) {
    if (contains(record.content, "SSN")) {
      reason = "PII rejected";
      return false;
    }
    record.metadata["privacy_checked"] = "true";
    return true;
  });

  bool threw = false;
  try {
    memory.remember_long_term("User SSN is 123-45-6789", test_scope());
  }
  catch (const std::invalid_argument&) {
    threw = true;
  }

  require(threw, "privacy filter should reject sensitive memory");
  require(!events.empty() && events.back().action == memory_audit_action::reject,
    "privacy rejection should emit audit event");

  const auto saved = memory.remember_long_term("User prefers concise answers", test_scope());
  require(saved.metadata.at("privacy_checked") == "true",
    "privacy filter should be able to annotate safe memory");
}

void test_default_retention_ttl_sets_expiry() {
  memory_policy policy;
  policy.default_long_term_ttl = std::chrono::seconds(60);
  memory_context memory(policy);
  memory.set_scope(test_scope());

  const auto saved = memory.remember_long_term("ttl memory", test_scope());
  require(saved.expires_at.has_value(), "default long-term TTL should set expiry");
  require(*saved.expires_at > std::chrono::system_clock::now(),
    "default long-term TTL should set future expiry");

  memory_record explicit_expiry;
  explicit_expiry.kind = memory_kind::long_term;
  explicit_expiry.content = "explicit expiry memory";
  explicit_expiry.scope = test_scope();
  explicit_expiry.expires_at = std::chrono::system_clock::now() + std::chrono::hours(2);
  const auto explicit_saved = memory.remember(explicit_expiry);
  require(explicit_saved.expires_at == explicit_expiry.expires_at,
    "default TTL should not overwrite explicit expiry");
}

void test_vector_index_rebuild_uses_batches() {
  memory_policy policy;
  policy.vector_rebuild_batch_size = 2;
  memory_context memory(policy);
  memory.set_scope(test_scope());

  memory.remember_long_term("first memory", test_scope());
  memory.remember_long_term("second memory", test_scope());
  memory.remember_long_term("third memory", test_scope());

  auto embeddings = std::make_shared<counting_batch_embedding_model>();
  auto index = std::make_shared<counting_vector_index>();
  memory.set_embedding_model(embeddings);
  memory.set_vector_index(index);

  const auto rebuild = memory.rebuild_vector_index_detailed();
  require(rebuild.scanned == 3, "batch rebuild should scan all records");
  require(rebuild.rebuilt == 3, "batch rebuild should rebuild all visible records");
  require(embeddings->single_calls == 0, "batch rebuild should use embed_batch");
  require(embeddings->batch_calls == 2, "batch rebuild should split embedding calls by batch size");
  require(embeddings->batch_sizes == std::vector<std::size_t>({ 2, 1 }),
    "batch rebuild should preserve configured embedding batch sizes");
  require(index->single_upserts == 0, "batch rebuild should use upsert_batch");
  require(index->batch_upserts == 2, "batch rebuild should split index upserts by batch size");
  require(index->batch_sizes == std::vector<std::size_t>({ 2, 1 }),
    "batch rebuild should preserve configured upsert batch sizes");
}

void test_hybrid_ranker_prefers_vector_match() {
  memory_context memory;
  memory.set_scope(test_scope());
  memory.set_embedding_model(std::make_shared<topic_embedding_model>());
  memory.set_vector_index(std::make_shared<in_memory_vector_index>());
  memory.set_ranker(std::make_shared<hybrid_memory_ranker>(
    hybrid_memory_ranker_policy {
      .vector_weight = 1.0,
      .lexical_weight = 0.0,
      .priority_weight = 0.0,
      .recency_weight = 0.0,
    }));

  memory.remember_long_term("Use explicit ownership in public APIs.", test_scope());
  memory.remember_long_term("Prefer Python notebooks for exploratory analysis.", test_scope());

  memory_query query;
  query.scope = test_scope();
  query.kinds = { memory_kind::long_term };
  query.text = "template interface design";
  query.limit = 1;

  const auto records = memory.recall(query);
  require(records.size() == 1, "hybrid ranker should return one vector-ranked record");
  require(contains(records.front().content, "explicit ownership"),
    "hybrid ranker should prefer highest vector score when vector weight dominates");
}

void test_hybrid_ranker_priority_and_minimum_vector_score() {
  hybrid_memory_ranker ranker(
    hybrid_memory_ranker_policy {
      .vector_weight = 0.0,
      .lexical_weight = 0.0,
      .priority_weight = 1.0,
      .recency_weight = 0.0,
      .minimum_vector_score = 0.5,
      .priority_scale = 10,
    });

  memory_record low_priority;
  low_priority.id = "low";
  low_priority.content = "low priority vector match";
  low_priority.priority = 1;
  low_priority.metadata["vector_score"] = "0.9";

  memory_record high_priority;
  high_priority.id = "high";
  high_priority.content = "high priority vector match";
  high_priority.priority = 9;
  high_priority.metadata["vector_score"] = "0.8";

  memory_record weak_vector;
  weak_vector.id = "weak";
  weak_vector.content = "weak vector match";
  weak_vector.priority = 10;
  weak_vector.metadata["vector_score"] = "0.1";

  memory_query query;
  query.limit = 3;
  const auto records = ranker.rank(query, { low_priority, high_priority, weak_vector });

  require(records.size() == 2, "hybrid ranker should filter by minimum vector score");
  require(records.front().id == "high",
    "hybrid ranker should honor priority weight after vector threshold");
}

void test_openai_embedding_model_request_and_parse() {
  auto http = std::make_shared<embedding_http_client>();
  openai_embedding_model model({
      .base_url = "https://embedding.example/",
      .api_key = "test-key",
      .model = "embedding-test",
      .timeout = 1234,
      .max_retries = 0,
    },
    http);

  const auto embedding = model.embed("hello memory");
  require(embedding.size() == 3, "embedding model should parse vector size");
  require(embedding[0] == 1.0F && embedding[1] == 0.0F,
    "embedding model should parse numeric vector values");
  require(http->requests.size() == 1, "embedding model should send one HTTP request");

  const auto& request = http->requests.front();
  require(request.method == "POST", "embedding request should use POST");
  require(request.url == "https://embedding.example/v1/embeddings",
    "embedding request should normalize base url and use embeddings endpoint");
  require(request.timeout == 1234, "embedding request should use configured timeout");
  require(contains(request.body, R"("model":"embedding-test")"),
    "embedding request should include model");
  require(contains(request.body, R"("input":"hello memory")"),
    "embedding request should include input");

  const auto has_auth = std::any_of(request.headers.begin(), request.headers.end(), [](const auto& header) {
    return header.first == "Authorization" && header.second == "Bearer test-key";
  });
  require(has_auth, "embedding request should include authorization header");
}

void test_openai_embedding_model_batch_request_and_parse() {
  auto http = std::make_shared<batch_embedding_http_client>();
  openai_embedding_model model({
      .base_url = "https://embedding.example",
      .model = "embedding-test",
      .max_retries = 0,
    },
    http);

  const auto embeddings = model.embed_batch({ "first", "second" });
  require(embeddings.size() == 2, "embedding model should parse two batch embeddings");
  require(embeddings[0][0] == 1.0F && embeddings[1][1] == 1.0F,
    "embedding model should order batch embeddings by response index");
  require(http->requests.size() == 1, "embedding batch should send one HTTP request");
  require(contains(http->requests.front().body, R"("input":["first","second"])"),
    "embedding batch request should send array input");
}

void test_openai_embedding_model_with_memory_context() {
  auto http = std::make_shared<embedding_http_client>();
  auto model = std::make_shared<openai_embedding_model>(
    openai_embedding_model_config {
      .base_url = "https://embedding.example",
      .model = "embedding-test",
      .max_retries = 0,
    },
    http);

  memory_context memory;
  memory.set_scope(test_scope());
  memory.set_embedding_model(model);
  memory.set_vector_index(std::make_shared<in_memory_vector_index>());

  memory.remember_long_term("Use explicit ownership in public APIs.", test_scope());

  memory_query query;
  query.scope = test_scope();
  query.kinds = { memory_kind::long_term };
  query.text = "interface design";
  query.limit = 1;

  const auto records = memory.recall(query);
  require(records.size() == 1, "OpenAI-compatible embedding model should support vector recall");
  require(contains(records.front().content, "explicit ownership"),
    "OpenAI-compatible embedding recall should return stored memory");
  require(http->requests.size() == 2,
    "embedding model should be called for write and query embeddings");
}

void test_qdrant_upsert_payload_includes_embedding_metadata() {
  auto http = std::make_shared<qdrant_capture_http_client>();
  qdrant_memory_index index(
    {
      .base_url = "http://qdrant.local/",
      .collection_name = "memory",
      .embedding_provider = "openai-compatible",
      .embedding_model = "embedding-test",
      .embedding_version = "2026-05-03",
      .index_schema_version = "2",
      .create_collection_if_missing = false,
    },
    http);

  memory_record record;
  record.id = "memory-1";
  record.kind = memory_kind::long_term;
  record.content = "payload metadata test";
  record.scope = test_scope();
  record.metadata["topic"] = "metadata";

  index.upsert_batch({ record }, { { 1.0F, 0.0F, 0.0F } });

  require(http->requests.size() == 1, "qdrant upsert should send one request");
  const auto body = nlohmann::json::parse(http->requests.front().body);
  const auto& payload = body["points"][0]["payload"];
  require(payload["embedding_provider"] == "openai-compatible",
    "qdrant payload should include embedding provider");
  require(payload["embedding_model"] == "embedding-test",
    "qdrant payload should include embedding model");
  require(payload["embedding_version"] == "2026-05-03",
    "qdrant payload should include embedding version");
  require(payload["embedding_dimension"] == 3,
    "qdrant payload should include embedding dimension");
  require(payload["index_schema_version"] == "2",
    "qdrant payload should include index schema version");
  require(payload["metadata"]["topic"] == "metadata",
    "qdrant payload should preserve memory metadata");
}

void test_qdrant_live_integration_when_configured() {
  const auto url = env_value("WUWE_QDRANT_URL");
  if (url.empty()) {
    println("[SKIP] qdrant live integration requires WUWE_QDRANT_URL");
    return;
  }

  qdrant_memory_index index({
    .base_url = url,
    .collection_name = env_value("WUWE_QDRANT_COLLECTION").empty()
      ? "wuwe_memory_live_test"
      : env_value("WUWE_QDRANT_COLLECTION"),
    .embedding_provider = "test",
    .embedding_model = "deterministic",
    .embedding_version = "live-test",
    .index_schema_version = "1",
  });

  const auto scope = test_scope("qdrant-live-" +
                                std::to_string(std::chrono::steady_clock::now()
                                  .time_since_epoch()
                                  .count()));

  memory_record ownership;
  ownership.id = "qdrant-live-ownership";
  ownership.kind = memory_kind::long_term;
  ownership.content = "Use explicit ownership in public APIs.";
  ownership.scope = scope;
  ownership.metadata["topic"] = "api-style";

  memory_record analysis;
  analysis.id = "qdrant-live-analysis";
  analysis.kind = memory_kind::long_term;
  analysis.content = "Prefer notebooks for exploratory analysis.";
  analysis.scope = scope;
  analysis.metadata["topic"] = "analysis";

  index.upsert_batch({ ownership, analysis }, { { 1.0F, 0.0F, 0.0F }, { 0.0F, 1.0F, 0.0F } });

  vector_memory_query query;
  query.embedding = { 1.0F, 0.0F, 0.0F };
  query.scope = scope;
  query.kinds = { memory_kind::long_term };
  query.filters = { { "topic", "api-style" } };
  query.limit = 1;

  const auto matches = index.search(query);
  require(matches.size() == 1, "qdrant live integration should return one match");
  require(matches.front().memory_id == ownership.id,
    "qdrant live integration should preserve memory_id payload");

  require(index.erase(ownership.id, scope), "qdrant live integration erase should succeed");
  index.clear(scope);
}

void test_sqlite_store() {
#if WUWE_HAS_SQLITE
  const auto path = std::filesystem::temp_directory_path() /
                    ("wuwe-memory-test-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                     ".sqlite3");

  const auto cleanup = [&] {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
  };

  cleanup();

  {
    sqlite_memory_store store(path);

    memory_record first;
    first.kind = memory_kind::long_term;
    first.content = "sqlite persisted one";
    first.scope = test_scope();
    first.metadata["topic"] = "sqlite";
    const auto saved = store.add(first);
    require(saved.id == "mem-1", "sqlite store should generate first id");

    const auto loaded = store.get(saved.id, test_scope());
    require(loaded.has_value(), "sqlite store should get saved record");
    require(loaded->content == "sqlite persisted one", "sqlite store should preserve content");
    require(loaded->metadata.at("topic") == "sqlite", "sqlite store should preserve metadata");

    memory_query query;
    query.scope = test_scope();
    query.kinds = { memory_kind::long_term };
    query.filters = { { "topic", "sqlite" } };
    query.text = "persisted";
    require(store.search(query).size() == 1, "sqlite store should search with filters");

    auto updated = *loaded;
    updated.content = "sqlite updated";
    require(store.update(updated), "sqlite store should update record");
    require(store.get(saved.id, test_scope())->content == "sqlite updated",
      "sqlite store should return updated content");

    memory_record expired;
    expired.kind = memory_kind::long_term;
    expired.content = "sqlite expired";
    expired.scope = test_scope();
    expired.metadata["topic"] = "expired";
    expired.expires_at = std::chrono::system_clock::now() - std::chrono::seconds(1);
    store.add(expired);

    memory_query expired_query;
    expired_query.scope = test_scope();
    expired_query.kinds = { memory_kind::long_term };
    expired_query.filters = { { "topic", "expired" } };
    expired_query.text = "expired";
    require(store.search(expired_query).empty(), "sqlite store should filter expired records");

    require(store.erase(saved.id, test_scope()), "sqlite store should erase by id and scope");
    require(!store.get(saved.id, test_scope()).has_value(), "sqlite erased record should be gone");
    require(store.clear(test_scope()) == 1, "sqlite clear should remove remaining scoped records");

    memory_record retained;
    retained.kind = memory_kind::long_term;
    retained.content = "sqlite retained";
    retained.scope = test_scope();
    const auto retained_saved = store.add(retained);
    require(retained_saved.id == "mem-3", "sqlite store should keep incrementing ids in process");
  }

  {
    sqlite_memory_store store(path);
    memory_record second;
    second.kind = memory_kind::long_term;
    second.content = "sqlite persisted two";
    second.scope = test_scope();
    const auto saved = store.add(second);
    require(saved.id == "mem-4", "sqlite store should continue generated ids after reload");
  }

  cleanup();
#else
  bool threw = false;
  try {
    sqlite_memory_store store("unused.sqlite3");
  }
  catch (const std::runtime_error&) {
    threw = true;
  }
  require(threw, "sqlite store should report unavailable when SQLite support is disabled");
#endif
}

void test_memory_tools_save_and_search() {
  memory_context memory;
  memory.set_scope(test_scope());
  memory_tool_provider provider(memory);

  const auto save_result = provider.invoke(
    "save_memory",
    R"({"content":"User prefers concise C++20 answers.","topic":"preference","sensitivity":"internal"})");
  require(!save_result.error_code, "save_memory should save valid long-term memory");

  const auto search_result = provider.invoke(
    "search_memory",
    R"({"content":"C++20 answers","topic":"preference","limit":3})");
  require(!search_result.error_code,
    "search_memory should search saved memory: " + search_result.content);
  require(contains(search_result.content, "User prefers concise C++20 answers."),
    "search_memory should return saved content");
  require(!contains(search_result.content, "tenant-a"),
    "search_memory should not expose internal scope fields");

  const auto secret_result = provider.invoke(
    "save_memory",
    R"({"content":"api token is abc","sensitivity":"secret"})");
  require(secret_result.error_code == std::errc::permission_denied,
    "save_memory should reject secret content");

  memory_context empty_scope_memory;
  memory_tool_provider empty_scope_provider(empty_scope_memory);
  const auto empty_scope_result = empty_scope_provider.invoke(
    "save_memory",
    R"({"content":"should fail"})");
  require(static_cast<bool>(empty_scope_result.error_code),
    "save_memory should reject invalid scope through memory policy");
}

void test_memory_tools_review_and_limit() {
  memory_policy policy;
  policy.max_long_term_records = 1;
  memory_context memory(policy);
  memory.set_scope(test_scope());

  memory_tool_options options;
  options.review_save = [](const memory_record& record, std::string& reason) {
    if (contains(record.content, "reject")) {
      reason = "review rejected content";
      return false;
    }
    return true;
  };

  memory_tool_provider provider(memory, options);
  const auto rejected = provider.invoke(
    "save_memory",
    R"({"content":"please reject this","topic":"review"})");
  require(rejected.error_code == std::errc::permission_denied,
    "save_memory should honor review callback");

  require(!provider.invoke(
    "save_memory",
    R"({"content":"first retained memory","topic":"limit"})").error_code,
    "save_memory should save first record");
  require(!provider.invoke(
    "save_memory",
    R"({"content":"second retained memory","topic":"limit"})").error_code,
    "save_memory should save second record");

  const auto search_result = provider.invoke(
    "search_memory",
    R"({"content":"retained","topic":"limit","limit":10})");
  require(!search_result.error_code, "search_memory should succeed with clamped limit");
  const auto json = nlohmann::json::parse(search_result.content);
  require(json.is_array() && json.size() == 1, "search_memory should clamp limit to policy");
}

void test_runner_memory_tool_call() {
  memory_context memory;
  memory.set_scope(test_scope());

  auto provider = std::make_shared<memory_tool_provider>(memory);
  tool_call_client client;
  llm_agent_runner runner(client, provider, &memory);

  const auto response = runner.complete("Remember my coding preference.");
  require(static_cast<bool>(response), "runner with memory tools should complete");
  require(client.calls == 2, "runner should call model again after memory tool result");

  memory_query query;
  query.scope = test_scope();
  query.kinds = { memory_kind::long_term };
  query.filters = { { "topic", "preference" } };
  query.text = "terse C++20";
  const auto records = memory.recall(query);
  require(!records.empty(), "save_memory tool call should write long-term memory");
}

void test_runner_observes_without_injecting_current_request_twice() {
  memory_context memory;
  memory.set_scope(test_scope());
  memory.observe({ .role = "user", .content = "repeat me once" });

  capture_client client;
  llm_agent_runner runner(client, &memory);
  const auto response = runner.complete("repeat me once");
  require(static_cast<bool>(response), "runner should return successful mock response");

  for (const auto& message : client.last_request.messages) {
    if (message.role == "system" && contains(message.content, "Relevant memory:")) {
      require(!contains(message.content, "repeat me once"),
        "runner request should not inject current user message as memory");
    }
  }

  memory_query query;
  query.scope = test_scope();
  query.kinds = { memory_kind::conversation };
  query.text = "assistant response";
  const auto records = memory.recall(query);
  require(!records.empty(), "runner should observe assistant response");
}

void run(const char* name, void (*test)()) {
  test();
  println("[PASS] {}", name);
}

} // namespace

int main() {
  try {
    run("scoped recall and isolation", test_scoped_recall_and_isolation);
    run("long-term scope required", test_long_term_scope_required);
    run("hidden secret and request dedupe", test_hidden_secret_and_request_dedupe);
    run("memory inspection list get and filters", test_memory_inspection_list_get_and_filters);
    run("memory inspection delete and clear", test_memory_inspection_delete_and_clear);
    run("conversation summarization callback", test_conversation_summarization_callback);
    run("conversation summarization can erase sources",
      test_conversation_summarization_can_erase_sources);
    run("budget and ranker", test_budget_and_ranker);
    run("file store reload and ids", test_file_store_reload_and_ids);
    run("vector index semantic recall", test_vector_index_semantic_recall);
    run("vector index lifecycle sync", test_vector_index_lifecycle_sync);
    run("vector index rebuild", test_vector_index_rebuild);
    run("vector index rebuild collects errors", test_vector_index_rebuild_collects_errors);
    run("index failure marks pending reindex on write",
      test_index_failure_marks_pending_reindex_on_write);
    run("reconcile pending reindex marks indexed",
      test_reconcile_pending_reindex_marks_indexed);
    run("compact expired memories", test_compact_expired_memories);
    run("audit sink records memory operations", test_audit_sink_records_memory_operations);
    run("privacy filter rejects sensitive memory", test_privacy_filter_rejects_sensitive_memory);
    run("default retention ttl sets expiry", test_default_retention_ttl_sets_expiry);
    run("vector index rebuild uses batches", test_vector_index_rebuild_uses_batches);
    run("hybrid ranker prefers vector match", test_hybrid_ranker_prefers_vector_match);
    run("hybrid ranker priority and minimum vector score",
      test_hybrid_ranker_priority_and_minimum_vector_score);
    run("openai embedding model request and parse", test_openai_embedding_model_request_and_parse);
    run("openai embedding model batch request and parse",
      test_openai_embedding_model_batch_request_and_parse);
    run("openai embedding model with memory context", test_openai_embedding_model_with_memory_context);
    run("qdrant upsert payload includes embedding metadata",
      test_qdrant_upsert_payload_includes_embedding_metadata);
    run("qdrant live integration when configured", test_qdrant_live_integration_when_configured);
    run("sqlite store", test_sqlite_store);
    run("memory tools save and search", test_memory_tools_save_and_search);
    run("memory tools review and limit", test_memory_tools_review_and_limit);
    run("runner memory tool call", test_runner_memory_tool_call);
    run("runner observes without duplicate injection", test_runner_observes_without_injecting_current_request_twice);
  }
  catch (const std::exception& ex) {
    println("[FAIL] {}", ex.what());
    return 1;
  }

  return 0;
}
