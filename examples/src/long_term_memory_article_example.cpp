#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

#include <wuwe/agent/memory/file_memory_store.hpp>
#include <wuwe/agent/memory/in_memory_store.hpp>
#include <wuwe/agent/memory/memory_context.hpp>
#include <wuwe/wuwe.h>

std::filesystem::path example_store_path() {
  const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch())
                       .count();
  return std::filesystem::temp_directory_path() /
         ("wuwe-long-term-memory-article-" + std::to_string(stamp) + ".jsonl");
}

int main() {
  namespace memory = wuwe::agent::memory;

  const auto store_path = example_store_path();

  memory::memory_scope durable_scope {
    .user_id = "article-reader",
    .application_id = "memory-article",
  };

  {
    auto short_term = std::make_shared<memory::in_memory_store>();
    auto long_term = std::make_shared<memory::file_memory_store>(store_path);

    memory::memory_context first_session(short_term, long_term);
    first_session.set_scope(durable_scope);

    first_session.remember_long_term(
      "The user prefers examples that explain architecture through concrete runtime behavior.",
      durable_scope,
      { { "topic", "writing-style" }, { "source", "user" } });

    first_session.remember_long_term(
      "In Wuwe memory management, memory_store is the authoritative source of truth.",
      durable_scope,
      { { "topic", "memory-architecture" }, { "source", "article-note" } });
  }

  auto short_term_after_restart = std::make_shared<memory::in_memory_store>();
  auto long_term_after_restart = std::make_shared<memory::file_memory_store>(store_path);

  memory::memory_policy policy;
  policy.max_long_term_records = 4;
  policy.max_memory_chars = 2200;

  memory::memory_context second_session(short_term_after_restart, long_term_after_restart, policy);
  second_session.set_scope(durable_scope);

  memory::memory_query query;
  query.scope = durable_scope;
  query.kinds = { memory::memory_kind::long_term };
  query.text = "How should the article explain memory architecture?";
  query.limit = 4;

  auto request = wuwe::make_message()
                 << ("system" < wuwe::says > "You are helping draft a technical article.")
                 << ("user" < wuwe::says > "Explain the long-term memory layer in one paragraph.");

  const auto augmented = second_session.augment(std::move(request), query.text);

  wuwe::println("Long-term memory example");
  wuwe::println("The first session wrote durable facts to: {}", store_path.string());
  wuwe::println("The second session re-opened the same store and recalled them.\n");

  wuwe::println("Recalled long-term records:");
  for (const auto& record : second_session.recall(query)) {
    wuwe::println("- {}", record.content);
  }

  wuwe::println("\nAugmented request:");
  for (const auto& message : augmented.messages) {
    wuwe::println("[{}] {}\n", message.role, message.content);
  }

  std::error_code ignored;
  std::filesystem::remove(store_path, ignored);
  wuwe::println("Temporary store file removed.");
}
