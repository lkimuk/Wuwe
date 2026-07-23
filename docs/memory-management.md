---
id: memory-management
title: Memory
description: Scope, store, retrieve, and inject agent memory.
---

# Memory

The memory module manages conversation, working, summary, long-term, and retrieved records. `memory_context` coordinates stores, scope, ranking, vector search, retention, privacy checks, auditing, and request augmentation.

## Minimal context

```cpp
namespace memory = wuwe::agent::memory;

memory::memory_context context({
  .max_recent_messages = 4,
  .max_working_records = 4,
  .max_long_term_records = 4,
  .max_memory_chars = 2000,
});

context.set_scope({
  .user_id = "local-user",
  .application_id = "my-app",
  .conversation_id = "session-1",
  .agent_id = "assistant",
});

context.observe({ .role = "user", .content = "Prefer concise answers." });
context.remember_working("The current task is documentation review.");
context.remember_long_term(
  "The user prefers explicit C++20 APIs.",
  context.scope());

auto request = wuwe::make_message()
  << ("user" < wuwe::says > "Review this design.");

const auto augmented = context.augment(std::move(request), "design review");
```

Long-term memory requires an application ID plus a tenant or user anchor by default. Scoped recall is also required by default. These checks prevent accidental cross-user retrieval when a host omits identity context.

## Stores and ranking

| Capability | Implementations |
| --- | --- |
| Record store | `in_memory_store`, `file_memory_store`, `sqlite_memory_store` |
| Lexical ranking | `lexical_memory_ranker` |
| Hybrid ranking | `hybrid_memory_ranker` |
| Vector index | `in_memory_vector_index`, `qdrant_memory_index` |
| Embeddings | `embedding_model` interface, `openai_embedding_model` |
| Model tools | `memory_tool_provider` with save and search operations |

`memory_context` can use separate short-term and long-term stores. If no stores are supplied, it uses in-memory stores and lexical ranking.

Vector search is optional. Attach an embedding model and vector index for semantic recall; the context can combine vector and lexical results through the hybrid ranker.

## Policy

`memory_policy` controls:

- record counts and context character/token budgets;
- per-record size;
- working and long-term TTLs;
- which memory kinds are injected;
- scope requirements;
- request deduplication;
- vector-index error behavior;
- system-message injection and its header.

Records marked hidden, or with metadata `sensitivity=secret`, are not exposed to the model context.

## Lifecycle and audit

The API supports update, erase, scoped clear, expired-record compaction, conversation summarization, and vector-index rebuild. Audit callbacks can record remember, recall, update, erase, clear, and maintenance operations.

Persistence does not replace host policy. The application is responsible for user consent, retention rules, encryption, backup, deletion guarantees, and access control.

See `examples/src/memory_example.cpp` and `examples/src/memory_vector_example.cpp`.
