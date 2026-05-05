# Memory Deployment Guide

This guide describes the recommended production shape for Wuwe memory management.

## Architecture

Use `memory_context` as the application-facing facade:

```text
application / runner / tools
  -> memory_context
     -> authoritative memory_store
     -> optional embedding_model
     -> optional vector_memory_index
     -> optional audit / privacy / retention hooks
```

The authoritative store owns the memory record text, scope, metadata, expiry, and audit-relevant fields. Qdrant is an optional vector index and can be rebuilt from the authoritative store.

Recommended production setup:

```text
short-term store: in_memory_store or application-managed store
long-term store: sqlite_memory_store, file_memory_store for local tools, or application database adapter
embedding: openai_embedding_model or compatible provider
vector index: qdrant_memory_index
ranker: hybrid_memory_ranker
```

## API Boundaries

`memory_context` is intentionally the public coordination surface. It owns cross-cutting behavior that must stay consistent:

- scope validation
- privacy filtering
- retention TTL application
- audit emission
- authoritative store writes
- vector index synchronization
- pending reindex marking
- context augmentation

Avoid calling `memory_store::update()`, `erase()`, or `clear()` directly from application code unless you also synchronize the vector index yourself. Prefer:

```cpp
memory.update(record);
memory.erase(id, scope, kind);
memory.clear(scope);
memory.compact_expired(query);
memory.reconcile_pending_reindex(query);
memory.rebuild_vector_index_detailed(query);
```

Short-term and long-term stores can generate the same local id, such as `mem-1`. Deletion and UI actions should carry both `id` and `kind`.

## Qdrant

Qdrant is a runtime service dependency, not a linked library dependency. Wuwe talks to it through REST.

Start Qdrant locally:

```powershell
E:\Qdrant\qdrant.exe
```

or with Docker:

```powershell
docker run -p 6333:6333 -p 6334:6334 `
  -v qdrant_storage:/qdrant/storage `
  qdrant/qdrant
```

Run the opt-in live test:

```powershell
$env:WUWE_QDRANT_URL="http://localhost:6333"
ctest --test-dir build-vcpkg -C Debug --output-on-failure -R memory_tests
```

Run the vector example:

```powershell
$env:WUWE_QDRANT_URL="http://localhost:6333"
.\build-vcpkg\examples\Debug\memory_vector_example.exe
```

Qdrant payload includes:

```text
memory_id
tenant_id
user_id
application_id
conversation_id
agent_id
kind
visibility
priority
created_at
updated_at
expires_at
metadata
embedding_provider
embedding_model
embedding_version
embedding_dimension
index_schema_version
```

## Embeddings

`openai_embedding_model` supports OpenAI-compatible `/v1/embeddings` providers:

```cpp
auto embeddings =
  std::make_shared<wuwe::agent::memory::openai_embedding_model>(
    wuwe::agent::memory::openai_embedding_model_config {
      .base_url = "https://api.openai.com",
      .api_key = "...",
      .model = "text-embedding-3-small",
    });

memory.set_embedding_model(embeddings);
```

Batch embedding is supported through `embedding_model::embed_batch()`. Providers that do not override it automatically fall back to per-record `embed()`.

## Privacy Policy

Use `set_privacy_filter()` to reject or annotate records before they are saved:

```cpp
memory.set_privacy_filter([](auto& record, std::string& reason) {
  if (record.metadata["sensitivity"] == "secret") {
    reason = "secret memory must not be persisted";
    return false;
  }

  record.metadata["privacy_checked"] = "true";
  return true;
});
```

The filter may mutate metadata, set `visibility`, set `expires_at`, or reject the record. Rejections emit an audit event and throw `std::invalid_argument`.

Recommended metadata keys:

```text
sensitivity=public|internal|private|secret
retention=session|temporary|project|user
topic=<application topic>
source=user|tool|application|conversation_summary
```

## Retention

Set default TTLs in `memory_policy`:

```cpp
wuwe::agent::memory::memory_policy policy;
policy.default_working_ttl = std::chrono::hours(1);
policy.default_long_term_ttl = std::chrono::hours(24 * 90);
```

Explicit `expires_at` values are preserved. Run compaction periodically:

```cpp
auto result = memory.compact_expired({
  .scope = scope,
  .limit = 1000,
});
```

`compact_expired()` deletes expired records through `memory_context::erase()`, so the vector index is kept in sync.

## Audit

Use `set_audit_sink()` to collect operational events:

```cpp
memory.set_audit_sink([](const auto& event) {
  write_audit_log({
    event.action,
    event.success,
    event.memory_id,
    event.kind,
    event.scope,
    event.message,
    event.metadata,
  });
});
```

Audit events are emitted for remember, update, erase, clear, compaction, rebuild, reconcile, summarization, and privacy rejection.

Do not store raw secrets in audit logs. Prefer ids, scope, topics, sensitivity labels, and operation status.

## Rebuild And Reconcile

Use detailed rebuild for planned migration or recovery:

```cpp
auto rebuild = memory.rebuild_vector_index_detailed({
  .scope = scope,
  .limit = 1000,
});
```

Use reconcile after embedding or Qdrant outage recovery:

```cpp
auto reconcile = memory.reconcile_pending_reindex({
  .scope = scope,
  .limit = 1000,
});
```

Records marked with `index_status=pending_reindex` are retried. Successful indexing changes the status to `indexed` and clears `index_error`.

## Operational Checklist

- Always set a scoped `application_id` and `user_id` or `tenant_id`.
- Use a privacy filter before enabling long-term memory tools.
- Configure retention defaults or require explicit `expires_at`.
- Use `hybrid_memory_ranker` for semantic recall.
- Run `compact_expired()` on a schedule.
- Run `reconcile_pending_reindex()` after provider outages.
- Treat Qdrant as rebuildable index state, not the source of truth.
- Back up the authoritative store, not only Qdrant storage.
