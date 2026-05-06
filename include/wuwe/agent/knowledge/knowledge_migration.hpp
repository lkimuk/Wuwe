#ifndef WUWE_AGENT_KNOWLEDGE_MIGRATION_HPP
#define WUWE_AGENT_KNOWLEDGE_MIGRATION_HPP

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <wuwe/agent/knowledge/knowledge_retriever.hpp>
#include <wuwe/agent/knowledge/knowledge_store.hpp>

namespace wuwe::agent::knowledge {

struct knowledge_store_audit_options {
  std::size_t expected_embedding_dimension {};
  std::size_t expected_index_schema_version {};
};

struct knowledge_store_audit_report {
  std::size_t documents {};
  std::size_t chunks {};
  std::size_t orphan_chunks {};
  std::size_t empty_documents {};
  std::size_t empty_chunks {};
  std::size_t embedding_dimension_mismatches {};
  std::size_t index_schema_mismatches {};
  std::vector<std::string> warnings;
};

struct knowledge_store_migration_options {
  bool clear_target {};
  bool erase_stale { true };
};

struct knowledge_store_migration_report {
  std::size_t source_documents {};
  knowledge_ingest_result ingest;
  knowledge_store_audit_report source_audit;
  knowledge_store_audit_report target_audit;
};

inline knowledge_store_audit_report audit_knowledge_store(
  const knowledge_store& store,
  knowledge_store_audit_options options = {}) {
  knowledge_store_audit_report report;

  const auto documents = store.list_documents();
  knowledge_query all_chunks;
  all_chunks.limit = 0;
  const auto chunks = store.list_chunks(all_chunks);

  report.documents = documents.size();
  report.chunks = chunks.size();

  std::set<std::string> document_ids;
  for (const auto& document : documents) {
    document_ids.insert(document.id);
    if (document.content.empty()) {
      ++report.empty_documents;
      report.warnings.push_back("document has empty content: " + document.id);
    }
  }

  for (const auto& chunk : chunks) {
    if (!document_ids.contains(chunk.document_id)) {
      ++report.orphan_chunks;
      report.warnings.push_back("chunk references missing document: " + chunk.id);
    }
    if (chunk.content.empty()) {
      ++report.empty_chunks;
      report.warnings.push_back("chunk has empty content: " + chunk.id);
    }

    if (options.expected_embedding_dimension != 0) {
      const auto dimension = chunk.metadata.find("embedding_dimension");
      if (dimension == chunk.metadata.end() ||
          dimension->second != std::to_string(options.expected_embedding_dimension)) {
        ++report.embedding_dimension_mismatches;
        report.warnings.push_back("chunk embedding dimension mismatch: " + chunk.id);
      }
    }

    if (options.expected_index_schema_version != 0) {
      const auto schema = chunk.metadata.find("index_schema_version");
      if (schema == chunk.metadata.end() ||
          schema->second != std::to_string(options.expected_index_schema_version)) {
        ++report.index_schema_mismatches;
        report.warnings.push_back("chunk index schema mismatch: " + chunk.id);
      }
    }
  }

  return report;
}

inline knowledge_store_migration_report migrate_knowledge_store(
  const knowledge_store& source,
  const knowledge_store& target_store,
  knowledge_retriever& target,
  knowledge_store_migration_options options = {},
  knowledge_store_audit_options audit_options = {}) {
  knowledge_store_migration_report report;
  report.source_audit = audit_knowledge_store(source, audit_options);

  auto documents = source.list_documents();
  report.source_documents = documents.size();

  if (options.clear_target) {
    target.clear();
  }

  report.ingest = target.ingest_incremental(documents, options.erase_stale);
  report.target_audit = audit_knowledge_store(target_store, audit_options);
  return report;
}

} // namespace wuwe::agent::knowledge

#endif // WUWE_AGENT_KNOWLEDGE_MIGRATION_HPP
