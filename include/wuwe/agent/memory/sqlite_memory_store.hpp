#ifndef WUWE_AGENT_MEMORY_SQLITE_MEMORY_STORE_HPP
#define WUWE_AGENT_MEMORY_SQLITE_MEMORY_STORE_HPP

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <wuwe/agent/memory/in_memory_store.hpp>

#if WUWE_HAS_SQLITE
#include <sqlite3.h>
#endif

namespace wuwe::agent::memory {

class sqlite_memory_store final : public memory_store {
public:
#if WUWE_HAS_SQLITE
  explicit sqlite_memory_store(std::filesystem::path path) : path_(std::move(path)) {
    open();
    initialize_schema();
    load_next_id();
  }

  ~sqlite_memory_store() override {
    if (db_) {
      sqlite3_close(db_);
    }
  }

  sqlite_memory_store(const sqlite_memory_store&) = delete;
  sqlite_memory_store& operator=(const sqlite_memory_store&) = delete;

  memory_record add(memory_record record) override {
    std::scoped_lock lock(mutex_);

    const auto now = std::chrono::system_clock::now();
    if (record.id.empty()) {
      record.id = "mem-" + std::to_string(++next_id_);
    }
    if (record.created_at == std::chrono::system_clock::time_point {}) {
      record.created_at = now;
    }
    record.updated_at = now;

    statement insert(db_,
      "INSERT INTO memory_records "
      "(id, kind, visibility, content, summary, tenant_id, user_id, application_id, "
      "conversation_id, agent_id, score, priority, created_at, updated_at, expires_at, metadata_json) "
      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    bind_record(insert.get(), record);
    step_done(insert.get(), "insert memory record");
    return record;
  }

  std::optional<memory_record> get(
    const std::string& id, const memory_scope& scope) const override {
    std::scoped_lock lock(mutex_);

    statement select(db_,
      "SELECT id, kind, visibility, content, summary, tenant_id, user_id, application_id, "
      "conversation_id, agent_id, score, priority, created_at, updated_at, expires_at, metadata_json "
      "FROM memory_records "
      "WHERE id = ? "
      "AND (? = '' OR tenant_id = ?) "
      "AND (? = '' OR user_id = ?) "
      "AND (? = '' OR application_id = ?) "
      "AND (? = '' OR conversation_id = ?) "
      "AND (? = '' OR agent_id = ?)");
    bind_text(select.get(), 1, id);
    bind_scope_filters(select.get(), scope, 2);

    const int rc = sqlite3_step(select.get());
    if (rc == SQLITE_DONE) {
      return std::nullopt;
    }
    if (rc != SQLITE_ROW) {
      throw_sqlite("get memory record", db_);
    }

    return read_record(select.get());
  }

  std::vector<memory_record> search(const memory_query& query) const override {
    std::scoped_lock lock(mutex_);

    statement select(db_,
      "SELECT id, kind, visibility, content, summary, tenant_id, user_id, application_id, "
      "conversation_id, agent_id, score, priority, created_at, updated_at, expires_at, metadata_json "
      "FROM memory_records "
      "WHERE (? = '' OR tenant_id = ?) "
      "AND (? = '' OR user_id = ?) "
      "AND (? = '' OR application_id = ?) "
      "AND (? = '' OR conversation_id = ?) "
      "AND (? = '' OR agent_id = ?)");
    bind_scope_filters(select.get(), query.scope, 1);

    std::vector<memory_record> result;
    while (true) {
      const int rc = sqlite3_step(select.get());
      if (rc == SQLITE_DONE) {
        break;
      }
      if (rc != SQLITE_ROW) {
        throw_sqlite("search memory records", db_);
      }

      auto record = read_record(select.get());
      if (!detail::contains_kind(query.kinds, record.kind)) {
        continue;
      }
      if (!query.include_expired && detail::is_expired(record)) {
        continue;
      }
      if (!detail::metadata_matches(record.metadata, query.filters)) {
        continue;
      }

      record.score = detail::lexical_score(query.text, record);
      result.push_back(std::move(record));
    }

    std::sort(result.begin(), result.end(), [](const memory_record& lhs, const memory_record& rhs) {
      if (lhs.score != rhs.score) {
        return lhs.score > rhs.score;
      }
      if (lhs.priority != rhs.priority) {
        return lhs.priority > rhs.priority;
      }
      return lhs.updated_at > rhs.updated_at;
    });

    if (result.size() > query.limit) {
      result.resize(query.limit);
    }
    return result;
  }

  bool update(memory_record record) override {
    std::scoped_lock lock(mutex_);

    const auto existing = get_unlocked(record.id, record.scope);
    if (!existing) {
      return false;
    }

    record.created_at = existing->created_at;
    record.updated_at = std::chrono::system_clock::now();

    statement update(db_,
      "UPDATE memory_records SET "
      "kind = ?, visibility = ?, content = ?, summary = ?, tenant_id = ?, user_id = ?, "
      "application_id = ?, conversation_id = ?, agent_id = ?, score = ?, priority = ?, "
      "created_at = ?, updated_at = ?, expires_at = ?, metadata_json = ? "
      "WHERE id = ?");

    bind_text(update.get(), 1, to_string(record.kind));
    bind_text(update.get(), 2, to_string(record.visibility));
    bind_text(update.get(), 3, record.content);
    bind_text(update.get(), 4, record.summary);
    bind_text(update.get(), 5, record.scope.tenant_id);
    bind_text(update.get(), 6, record.scope.user_id);
    bind_text(update.get(), 7, record.scope.application_id);
    bind_text(update.get(), 8, record.scope.conversation_id);
    bind_text(update.get(), 9, record.scope.agent_id);
    sqlite3_bind_double(update.get(), 10, record.score);
    sqlite3_bind_int(update.get(), 11, record.priority);
    sqlite3_bind_int64(update.get(), 12, to_unix_millis(record.created_at));
    sqlite3_bind_int64(update.get(), 13, to_unix_millis(record.updated_at));
    bind_optional_time(update.get(), 14, record.expires_at);
    bind_text(update.get(), 15, metadata_to_json(record.metadata));
    bind_text(update.get(), 16, record.id);

    step_done(update.get(), "update memory record");
    return sqlite3_changes(db_) > 0;
  }

  bool erase(const std::string& id, const memory_scope& scope) override {
    std::scoped_lock lock(mutex_);

    statement erase(db_,
      "DELETE FROM memory_records "
      "WHERE id = ? "
      "AND (? = '' OR tenant_id = ?) "
      "AND (? = '' OR user_id = ?) "
      "AND (? = '' OR application_id = ?) "
      "AND (? = '' OR conversation_id = ?) "
      "AND (? = '' OR agent_id = ?)");
    bind_text(erase.get(), 1, id);
    bind_scope_filters(erase.get(), scope, 2);

    step_done(erase.get(), "erase memory record");
    return sqlite3_changes(db_) > 0;
  }

  std::size_t clear(const memory_scope& scope) override {
    std::scoped_lock lock(mutex_);

    statement clear(db_,
      "DELETE FROM memory_records "
      "WHERE (? = '' OR tenant_id = ?) "
      "AND (? = '' OR user_id = ?) "
      "AND (? = '' OR application_id = ?) "
      "AND (? = '' OR conversation_id = ?) "
      "AND (? = '' OR agent_id = ?)");
    bind_scope_filters(clear.get(), scope, 1);

    step_done(clear.get(), "clear memory records");
    return static_cast<std::size_t>(sqlite3_changes(db_));
  }

private:
  class statement {
  public:
    statement(sqlite3* db, const char* sql) : db_(db) {
      if (sqlite3_prepare_v2(db_, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
        throw_sqlite("prepare sqlite statement", db_);
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
      throw std::runtime_error("failed to open sqlite memory store: " + message);
    }
  }

  void initialize_schema() {
    exec(
      "CREATE TABLE IF NOT EXISTS memory_records ("
      "id TEXT PRIMARY KEY,"
      "kind TEXT NOT NULL,"
      "visibility TEXT NOT NULL,"
      "content TEXT NOT NULL,"
      "summary TEXT NOT NULL,"
      "tenant_id TEXT NOT NULL,"
      "user_id TEXT NOT NULL,"
      "application_id TEXT NOT NULL,"
      "conversation_id TEXT NOT NULL,"
      "agent_id TEXT NOT NULL,"
      "score REAL NOT NULL,"
      "priority INTEGER NOT NULL,"
      "created_at INTEGER NOT NULL,"
      "updated_at INTEGER NOT NULL,"
      "expires_at INTEGER,"
      "metadata_json TEXT NOT NULL"
      ")");
    exec(
      "CREATE INDEX IF NOT EXISTS idx_memory_scope "
      "ON memory_records (tenant_id, user_id, application_id, conversation_id, agent_id)");
    exec("CREATE INDEX IF NOT EXISTS idx_memory_kind ON memory_records (kind)");
    exec("CREATE INDEX IF NOT EXISTS idx_memory_expiry ON memory_records (expires_at)");
  }

  void exec(const char* sql) const {
    char* error = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &error) != SQLITE_OK) {
      std::string message = error ? error : "unknown sqlite error";
      sqlite3_free(error);
      throw std::runtime_error("sqlite memory store exec failed: " + message);
    }
  }

  void load_next_id() {
    statement select(db_, "SELECT id FROM memory_records WHERE id LIKE 'mem-%'");
    while (true) {
      const int rc = sqlite3_step(select.get());
      if (rc == SQLITE_DONE) {
        break;
      }
      if (rc != SQLITE_ROW) {
        throw_sqlite("load next memory id", db_);
      }

      const std::string id = column_text(select.get(), 0);
      const auto numeric = id.substr(4);
      try {
        next_id_ = (std::max)(next_id_, static_cast<std::size_t>(std::stoull(numeric)));
      }
      catch (...) {
      }
    }
  }

  std::optional<memory_record> get_unlocked(
    const std::string& id, const memory_scope& scope) const {
    statement select(db_,
      "SELECT id, kind, visibility, content, summary, tenant_id, user_id, application_id, "
      "conversation_id, agent_id, score, priority, created_at, updated_at, expires_at, metadata_json "
      "FROM memory_records "
      "WHERE id = ? "
      "AND (? = '' OR tenant_id = ?) "
      "AND (? = '' OR user_id = ?) "
      "AND (? = '' OR application_id = ?) "
      "AND (? = '' OR conversation_id = ?) "
      "AND (? = '' OR agent_id = ?)");
    bind_text(select.get(), 1, id);
    bind_scope_filters(select.get(), scope, 2);

    const int rc = sqlite3_step(select.get());
    if (rc == SQLITE_DONE) {
      return std::nullopt;
    }
    if (rc != SQLITE_ROW) {
      throw_sqlite("get memory record", db_);
    }
    return read_record(select.get());
  }

  static void bind_record(sqlite3_stmt* stmt, const memory_record& record) {
    bind_text(stmt, 1, record.id);
    bind_text(stmt, 2, to_string(record.kind));
    bind_text(stmt, 3, to_string(record.visibility));
    bind_text(stmt, 4, record.content);
    bind_text(stmt, 5, record.summary);
    bind_text(stmt, 6, record.scope.tenant_id);
    bind_text(stmt, 7, record.scope.user_id);
    bind_text(stmt, 8, record.scope.application_id);
    bind_text(stmt, 9, record.scope.conversation_id);
    bind_text(stmt, 10, record.scope.agent_id);
    sqlite3_bind_double(stmt, 11, record.score);
    sqlite3_bind_int(stmt, 12, record.priority);
    sqlite3_bind_int64(stmt, 13, to_unix_millis(record.created_at));
    sqlite3_bind_int64(stmt, 14, to_unix_millis(record.updated_at));
    bind_optional_time(stmt, 15, record.expires_at);
    bind_text(stmt, 16, metadata_to_json(record.metadata));
  }

  static void bind_scope_filters(sqlite3_stmt* stmt, const memory_scope& scope, int start) {
    bind_text(stmt, start, scope.tenant_id);
    bind_text(stmt, start + 1, scope.tenant_id);
    bind_text(stmt, start + 2, scope.user_id);
    bind_text(stmt, start + 3, scope.user_id);
    bind_text(stmt, start + 4, scope.application_id);
    bind_text(stmt, start + 5, scope.application_id);
    bind_text(stmt, start + 6, scope.conversation_id);
    bind_text(stmt, start + 7, scope.conversation_id);
    bind_text(stmt, start + 8, scope.agent_id);
    bind_text(stmt, start + 9, scope.agent_id);
  }

  static memory_record read_record(sqlite3_stmt* stmt) {
    memory_record record;
    record.id = column_text(stmt, 0);
    record.kind = kind_from_storage(column_text(stmt, 1));
    record.visibility = visibility_from_storage(column_text(stmt, 2));
    record.content = column_text(stmt, 3);
    record.summary = column_text(stmt, 4);
    record.scope.tenant_id = column_text(stmt, 5);
    record.scope.user_id = column_text(stmt, 6);
    record.scope.application_id = column_text(stmt, 7);
    record.scope.conversation_id = column_text(stmt, 8);
    record.scope.agent_id = column_text(stmt, 9);
    record.score = sqlite3_column_double(stmt, 10);
    record.priority = sqlite3_column_int(stmt, 11);
    record.created_at = from_unix_millis(sqlite3_column_int64(stmt, 12));
    record.updated_at = from_unix_millis(sqlite3_column_int64(stmt, 13));
    if (sqlite3_column_type(stmt, 14) != SQLITE_NULL) {
      record.expires_at = from_unix_millis(sqlite3_column_int64(stmt, 14));
    }
    record.metadata = metadata_from_json(column_text(stmt, 15));
    return record;
  }

  static std::int64_t to_unix_millis(std::chrono::system_clock::time_point value) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(value.time_since_epoch()).count();
  }

  static std::chrono::system_clock::time_point from_unix_millis(std::int64_t value) {
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(value));
  }

  static void bind_optional_time(
    sqlite3_stmt* stmt,
    int index,
    std::optional<std::chrono::system_clock::time_point> value) {
    if (value) {
      sqlite3_bind_int64(stmt, index, to_unix_millis(*value));
      return;
    }
    sqlite3_bind_null(stmt, index);
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

  static std::string metadata_to_json(const std::map<std::string, std::string>& metadata) {
    return nlohmann::json(metadata).dump();
  }

  static std::map<std::string, std::string> metadata_from_json(const std::string& text) {
    if (text.empty()) {
      return {};
    }
    const auto json = nlohmann::json::parse(text);
    if (!json.is_object()) {
      return {};
    }
    return json.get<std::map<std::string, std::string>>();
  }

  static memory_kind kind_from_storage(const std::string& value) {
    if (value == "conversation") {
      return memory_kind::conversation;
    }
    if (value == "working") {
      return memory_kind::working;
    }
    if (value == "summary") {
      return memory_kind::summary;
    }
    if (value == "long_term") {
      return memory_kind::long_term;
    }
    if (value == "retrieved") {
      return memory_kind::retrieved;
    }
    throw std::invalid_argument("invalid memory kind: " + value);
  }

  static memory_visibility visibility_from_storage(const std::string& value) {
    if (value == "visible") {
      return memory_visibility::visible;
    }
    if (value == "hidden") {
      return memory_visibility::hidden;
    }
    throw std::invalid_argument("invalid memory visibility: " + value);
  }

  static void step_done(sqlite3_stmt* stmt, const char* action) {
    const int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
      throw_sqlite(action, sqlite3_db_handle(stmt));
    }
  }

  [[noreturn]] static void throw_sqlite(const char* action, sqlite3* db) {
    throw std::runtime_error(
      std::string("sqlite memory store failed to ") + action + ": " + sqlite3_errmsg(db));
  }

private:
  std::filesystem::path path_;
  sqlite3* db_ {};
  mutable std::mutex mutex_;
  std::size_t next_id_ { 0 };
#else
  explicit sqlite_memory_store(std::filesystem::path) {
    throw std::runtime_error(
      "sqlite_memory_store requires WUWE_ENABLE_SQLITE and a SQLite3 development library");
  }

  memory_record add(memory_record) override {
    throw_unavailable();
  }

  std::optional<memory_record> get(const std::string&, const memory_scope&) const override {
    throw_unavailable();
  }

  std::vector<memory_record> search(const memory_query&) const override {
    throw_unavailable();
  }

  bool update(memory_record) override {
    throw_unavailable();
  }

  bool erase(const std::string&, const memory_scope&) override {
    throw_unavailable();
  }

  std::size_t clear(const memory_scope&) override {
    throw_unavailable();
  }

private:
  [[noreturn]] static void throw_unavailable() {
    throw std::runtime_error(
      "sqlite_memory_store requires WUWE_ENABLE_SQLITE and a SQLite3 development library");
  }
#endif
};

} // namespace wuwe::agent::memory

#endif // WUWE_AGENT_MEMORY_SQLITE_MEMORY_STORE_HPP
