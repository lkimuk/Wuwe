#ifndef WUWE_AGENT_KNOWLEDGE_SQLITE_KNOWLEDGE_INDEX_HPP
#define WUWE_AGENT_KNOWLEDGE_SQLITE_KNOWLEDGE_INDEX_HPP

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <wuwe/agent/knowledge/file_knowledge_index.hpp>

#if WUWE_HAS_SQLITE
#include <sqlite3.h>
#endif

namespace wuwe::agent::knowledge {

class sqlite_knowledge_index final : public knowledge_index {
public:
#if WUWE_HAS_SQLITE
  explicit sqlite_knowledge_index(std::filesystem::path path) : path_(std::move(path)) {
    open();
    initialize_schema();
  }

  ~sqlite_knowledge_index() override {
    if (db_) {
      sqlite3_close(db_);
    }
  }

  sqlite_knowledge_index(const sqlite_knowledge_index&) = delete;
  sqlite_knowledge_index& operator=(const sqlite_knowledge_index&) = delete;

  void upsert(const knowledge_chunk& chunk, const std::vector<float>& embedding) override {
    std::scoped_lock lock(mutex_);
    upsert_unlocked(chunk, embedding);
  }

  void upsert_batch(
    const std::vector<knowledge_chunk>& chunks,
    const std::vector<std::vector<float>>& embeddings) override {
    if (chunks.size() != embeddings.size()) {
      throw std::invalid_argument("sqlite_knowledge_index upsert_batch size mismatch");
    }

    std::scoped_lock lock(mutex_);
    exec("BEGIN IMMEDIATE");
    try {
      for (std::size_t index = 0; index < chunks.size(); ++index) {
        upsert_unlocked(chunks[index], embeddings[index]);
      }
      exec("COMMIT");
    }
    catch (...) {
      exec("ROLLBACK");
      throw;
    }
  }

  std::vector<knowledge_result> search(
    const knowledge_query& query,
    const std::vector<float>& embedding) const override {
    std::scoped_lock lock(mutex_);

    statement select(db_,
      "SELECT chunk_json, embedding_json FROM knowledge_index");

    std::vector<knowledge_result> result;
    while (true) {
      const int rc = sqlite3_step(select.get());
      if (rc == SQLITE_DONE) {
        break;
      }
      if (rc != SQLITE_ROW) {
        throw_sqlite("search knowledge index", db_);
      }

      auto chunk = detail::knowledge_chunk_from_json(
        nlohmann::json::parse(column_text(select.get(), 0)));
      if (!metadata_matches(chunk.metadata, query.filters) ||
          !metadata_access_matches(chunk.metadata, query.access)) {
        continue;
      }

      const auto stored_embedding = detail::knowledge_embedding_from_json(
        nlohmann::json::parse(column_text(select.get(), 1)));
      const auto vector_score =
        ::wuwe::agent::memory::vector_detail::cosine_similarity(embedding, stored_embedding);
      const auto lexical_score = detail::lexical_knowledge_score(query.text, chunk);
      const auto score =
        query.vector_weight * vector_score + query.lexical_weight * lexical_score;
      if (score < query.minimum_score) {
        continue;
      }

      result.push_back({
        .chunk = std::move(chunk),
        .score = score,
        .vector_score = vector_score,
        .lexical_score = lexical_score,
      });
    }

    std::sort(result.begin(), result.end(), [](const knowledge_result& lhs,
                                                const knowledge_result& rhs) {
      if (lhs.score != rhs.score) {
        return lhs.score > rhs.score;
      }
      if (lhs.chunk.document_id != rhs.chunk.document_id) {
        return lhs.chunk.document_id < rhs.chunk.document_id;
      }
      return lhs.chunk.start_offset < rhs.chunk.start_offset;
    });

    if (query.limit != 0 && result.size() > query.limit) {
      result.resize(query.limit);
    }
    return result;
  }

  bool erase_document(const std::string& document_id) override {
    std::scoped_lock lock(mutex_);
    statement erase(db_, "DELETE FROM knowledge_index WHERE document_id = ?");
    bind_text(erase.get(), 1, document_id);
    step_done(erase.get(), "erase knowledge document from index");
    return sqlite3_changes(db_) > 0;
  }

  void clear() override {
    std::scoped_lock lock(mutex_);
    exec("DELETE FROM knowledge_index");
  }

private:
  class statement {
  public:
    statement(sqlite3* db, const char* sql) : db_(db) {
      if (sqlite3_prepare_v2(db_, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
        throw_sqlite("prepare sqlite knowledge statement", db_);
      }
    }

    ~statement() {
      if (stmt_) {
        sqlite3_finalize(stmt_);
      }
    }

    statement(const statement&) = delete;
    statement& operator=(const statement&) = delete;

    sqlite3_stmt* get() const noexcept {
      return stmt_;
    }

  private:
    sqlite3* db_ {};
    sqlite3_stmt* stmt_ {};
  };

  void open() {
    if (path_.has_parent_path()) {
      std::filesystem::create_directories(path_.parent_path());
    }

    if (sqlite3_open(path_.string().c_str(), &db_) != SQLITE_OK) {
      const std::string message = db_ ? sqlite3_errmsg(db_) : "unknown sqlite open error";
      if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
      }
      throw std::runtime_error("failed to open sqlite knowledge index: " + message);
    }
  }

  void initialize_schema() {
    exec(
      "CREATE TABLE IF NOT EXISTS knowledge_index ("
      "chunk_id TEXT PRIMARY KEY,"
      "document_id TEXT NOT NULL,"
      "source_uri TEXT NOT NULL,"
      "start_offset INTEGER NOT NULL,"
      "chunk_json TEXT NOT NULL,"
      "embedding_json TEXT NOT NULL"
      ")");
    exec(
      "CREATE INDEX IF NOT EXISTS idx_knowledge_index_document "
      "ON knowledge_index (document_id, start_offset)");
  }

  void upsert_unlocked(const knowledge_chunk& chunk, const std::vector<float>& embedding) {
    statement insert(db_,
      "INSERT OR REPLACE INTO knowledge_index "
      "(chunk_id, document_id, source_uri, start_offset, chunk_json, embedding_json) "
      "VALUES (?, ?, ?, ?, ?, ?)");
    bind_text(insert.get(), 1, chunk.id);
    bind_text(insert.get(), 2, chunk.document_id);
    bind_text(insert.get(), 3, chunk.source_uri);
    sqlite3_bind_int64(insert.get(), 4, static_cast<sqlite3_int64>(chunk.start_offset));
    bind_text(insert.get(), 5, detail::knowledge_chunk_to_json(chunk).dump());
    bind_text(insert.get(), 6, detail::knowledge_embedding_to_json(embedding).dump());
    step_done(insert.get(), "upsert knowledge chunk");
  }

  void exec(const char* sql) const {
    char* error = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &error) != SQLITE_OK) {
      std::string message = error ? error : "unknown sqlite error";
      sqlite3_free(error);
      throw std::runtime_error("sqlite knowledge index exec failed: " + message);
    }
  }

  static void bind_text(sqlite3_stmt* stmt, int index, const std::string& value) {
    sqlite3_bind_text(stmt, index, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
  }

  static std::string column_text(sqlite3_stmt* stmt, int index) {
    const auto* text = sqlite3_column_text(stmt, index);
    if (!text) {
      return {};
    }
    return reinterpret_cast<const char*>(text);
  }

  static void step_done(sqlite3_stmt* stmt, const char* action) {
    const int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
      throw_sqlite(action, sqlite3_db_handle(stmt));
    }
  }

  [[noreturn]] static void throw_sqlite(const char* action, sqlite3* db) {
    throw std::runtime_error(
      std::string("sqlite knowledge index failed to ") + action + ": " + sqlite3_errmsg(db));
  }

  std::filesystem::path path_;
  sqlite3* db_ {};
  mutable std::mutex mutex_;
#else
public:
  explicit sqlite_knowledge_index(std::filesystem::path) {
    throw std::runtime_error(
      "sqlite_knowledge_index requires WUWE_ENABLE_SQLITE and a SQLite3 development library");
  }

  void upsert(const knowledge_chunk&, const std::vector<float>&) override {
    throw_unavailable();
  }

  std::vector<knowledge_result> search(
    const knowledge_query&,
    const std::vector<float>&) const override {
    throw_unavailable();
  }

  bool erase_document(const std::string&) override {
    throw_unavailable();
  }

  void clear() override {
    throw_unavailable();
  }

private:
  [[noreturn]] static void throw_unavailable() {
    throw std::runtime_error(
      "sqlite_knowledge_index requires WUWE_ENABLE_SQLITE and a SQLite3 development library");
  }
#endif
};

} // namespace wuwe::agent::knowledge

#endif // WUWE_AGENT_KNOWLEDGE_SQLITE_KNOWLEDGE_INDEX_HPP
