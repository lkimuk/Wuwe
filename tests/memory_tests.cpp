#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <wuwe/agent/llm/llm_agent_runner.h>
#include <wuwe/agent/memory/file_memory_store.hpp>
#include <wuwe/agent/memory/in_memory_store.hpp>
#include <wuwe/agent/memory/memory_context.hpp>
#include <wuwe/agent/memory/sqlite_memory_store.hpp>

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
  std::cout << "[PASS] " << name << '\n';
}

} // namespace

int main() {
  try {
    run("scoped recall and isolation", test_scoped_recall_and_isolation);
    run("long-term scope required", test_long_term_scope_required);
    run("hidden secret and request dedupe", test_hidden_secret_and_request_dedupe);
    run("budget and ranker", test_budget_and_ranker);
    run("file store reload and ids", test_file_store_reload_and_ids);
    run("sqlite store", test_sqlite_store);
    run("runner observes without duplicate injection", test_runner_observes_without_injecting_current_request_twice);
  }
  catch (const std::exception& ex) {
    std::cerr << "[FAIL] " << ex.what() << '\n';
    return 1;
  }

  return 0;
}
