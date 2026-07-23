---
id: memory-deployment
title: Memory deployment
description: Choose a memory store and vector topology for local or service deployments.
---

# Memory deployment

Choose memory components from the application's durability, scale, and isolation requirements. The framework does not silently promote one backend to another.

## Recommended topologies

| Deployment | Record store | Semantic index | Notes |
| --- | --- | --- | --- |
| Tests or ephemeral agents | In-memory | In-memory | No durability; simplest lifecycle |
| Single-user desktop or CLI | File or SQLite | In-memory, rebuilt on startup, or Qdrant | Keep scope and retention explicit |
| Local durable application | SQLite | Qdrant when semantic volume grows | SQLite handles records; Qdrant handles vector search |
| Multi-instance service | Application database adapter | Qdrant or application vector service | Implement shared consistency, tenancy, backup, and migrations in the host |

## SQLite boundary

`sqlite_memory_store` provides durable CRUD and scoped queries when Wuwe is built with SQLite. The official 0.1.0 presets require SQLite.

It is intended for local persistence. It does not provide a distributed coordination model, a built-in migration service, or a vector extension. Memory ranking can still happen in C++ after scoped records are read.

Create it only when the compile-time capability is enabled:

```cpp
#if WUWE_HAS_SQLITE
auto durable = std::make_shared<wuwe::agent::memory::sqlite_memory_store>(
  "state/memory.db");
#endif
```

## Qdrant boundary

`qdrant_memory_index` stores embeddings in an external Qdrant service over HTTP. The host supplies the endpoint, collection, optional API key, and deployment policy. Wuwe does not start or manage Qdrant.

Use the record ID and full memory scope consistently across the record store and vector index. Rebuild the vector index after embedding-model, dimension, or indexing-policy changes.

## Operational requirements

Before production use, define:

- tenant, user, application, conversation, and agent scope rules;
- maximum retention and deletion behavior;
- encryption at rest and secret handling;
- backup and restore ownership;
- embedding provider, model, and dimension;
- index rebuild and failure policy;
- observability and audit retention.

For build-time SQLite behavior, see [Dependencies](dependencies.md).
