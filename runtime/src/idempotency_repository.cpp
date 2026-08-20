#include "idempotency_repository.hpp"

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

#include <wuwe/agent/core/filesystem.hpp>
#include <wuwe/agent/core/sqlite_schema.hpp>

#if WUWE_HAS_SQLITE
#include <sqlite3.h>
#endif

namespace wuwe::runtime_host::detail {
namespace {

struct memory_record {
  std::string fingerprint;
  std::optional<nlohmann::json> result;
  std::chrono::steady_clock::time_point created_at { std::chrono::steady_clock::now() };
};

class memory_idempotency_repository final : public idempotency_repository {
public:
  explicit memory_idempotency_repository(std::size_t capacity) : capacity_(capacity) {
    if (capacity_ == 0)
      throw std::invalid_argument("idempotency repository capacity must be positive");
  }

  idempotency_claim claim(const std::string& key, const std::string& fingerprint) override {
    std::scoped_lock lock(mutex_);
    if (const auto found = records_.find(key); found != records_.end()) {
      if (found->second.fingerprint != fingerprint)
        return { .status = idempotency_claim_status::conflict };
      if (found->second.result) {
        return {
          .status = idempotency_claim_status::replay,
          .result = *found->second.result,
        };
      }
      return { .status = idempotency_claim_status::pending };
    }
    prune_completed();
    if (records_.size() >= capacity_)
      return { .status = idempotency_claim_status::capacity_exceeded };
    records_.emplace(key, memory_record { .fingerprint = fingerprint });
    return { .status = idempotency_claim_status::acquired };
  }

  void complete(const std::string& key, const std::string& fingerprint,
    const nlohmann::json& result) override {
    std::scoped_lock lock(mutex_);
    const auto found = records_.find(key);
    if (found == records_.end() || found->second.fingerprint != fingerprint)
      throw std::runtime_error("idempotency claim disappeared before completion");
    found->second.result = result;
  }

  void erase(const std::string& key, const std::string& fingerprint) override {
    std::scoped_lock lock(mutex_);
    const auto found = records_.find(key);
    if (found != records_.end() && found->second.fingerprint == fingerprint)
      records_.erase(found);
  }

private:
  void prune_completed() {
    while (records_.size() >= capacity_) {
      auto oldest = records_.end();
      for (auto current = records_.begin(); current != records_.end(); ++current) {
        if (!current->second.result)
          continue;
        if (oldest == records_.end() || current->second.created_at < oldest->second.created_at)
          oldest = current;
      }
      if (oldest == records_.end())
        return;
      records_.erase(oldest);
    }
  }

  const std::size_t capacity_;
  std::mutex mutex_;
  std::map<std::string, memory_record> records_;
};

#if WUWE_HAS_SQLITE

std::int64_t unix_milliseconds() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch())
    .count();
}

class sqlite_idempotency_repository final : public idempotency_repository {
public:
  static constexpr int latest_schema_version = 1;

  sqlite_idempotency_repository(std::filesystem::path path, std::size_t capacity)
      : path_(std::move(path)), capacity_(capacity) {
    if (path_.empty())
      throw std::invalid_argument("SQLite idempotency repository requires a path");
    if (capacity_ == 0)
      throw std::invalid_argument("idempotency repository capacity must be positive");
    try {
      open();
      initialize_schema();
    }
    catch (...) {
      close();
      throw;
    }
  }

  ~sqlite_idempotency_repository() override {
    close();
  }

  sqlite_idempotency_repository(const sqlite_idempotency_repository&) = delete;
  sqlite_idempotency_repository& operator=(const sqlite_idempotency_repository&) = delete;

  idempotency_claim claim(const std::string& key, const std::string& fingerprint) override {
    std::scoped_lock lock(mutex_);
    transaction tx(*this);
    statement select(db_,
      "SELECT fingerprint, state, result_json FROM wuwe_runtime_idempotency WHERE key = ?");
    bind_text(select.get(), 1, key);
    const auto rc = sqlite3_step(select.get());
    if (rc == SQLITE_ROW) {
      const auto stored_fingerprint = column_text(select.get(), 0);
      const auto state = sqlite3_column_int(select.get(), 1);
      const auto encoded_result = column_text(select.get(), 2);
      tx.commit();
      if (stored_fingerprint != fingerprint)
        return { .status = idempotency_claim_status::conflict };
      if (state == completed_state) {
        return {
          .status = idempotency_claim_status::replay,
          .result = nlohmann::json::parse(encoded_result),
        };
      }
      if (state != pending_state)
        throw std::runtime_error("SQLite idempotency repository contains an invalid state");
      return { .status = idempotency_claim_status::pending };
    }
    if (rc != SQLITE_DONE)
      throw_sqlite("read idempotency claim");

    prune_completed();
    if (record_count() >= capacity_) {
      tx.rollback();
      return { .status = idempotency_claim_status::capacity_exceeded };
    }
    const auto now = unix_milliseconds();
    statement insert(db_,
      "INSERT INTO wuwe_runtime_idempotency "
      "(key, fingerprint, state, result_json, created_at_ms, updated_at_ms) "
      "VALUES (?, ?, ?, NULL, ?, ?)");
    bind_text(insert.get(), 1, key);
    bind_text(insert.get(), 2, fingerprint);
    sqlite3_bind_int(insert.get(), 3, pending_state);
    sqlite3_bind_int64(insert.get(), 4, now);
    sqlite3_bind_int64(insert.get(), 5, now);
    step_done(insert.get(), "create idempotency claim");
    tx.commit();
    return { .status = idempotency_claim_status::acquired };
  }

  void complete(const std::string& key, const std::string& fingerprint,
    const nlohmann::json& result) override {
    std::scoped_lock lock(mutex_);
    transaction tx(*this);
    statement update(db_,
      "UPDATE wuwe_runtime_idempotency "
      "SET state = ?, result_json = ?, updated_at_ms = ? "
      "WHERE key = ? AND fingerprint = ? AND state = ?");
    sqlite3_bind_int(update.get(), 1, completed_state);
    bind_text(update.get(), 2, result.dump());
    sqlite3_bind_int64(update.get(), 3, unix_milliseconds());
    bind_text(update.get(), 4, key);
    bind_text(update.get(), 5, fingerprint);
    sqlite3_bind_int(update.get(), 6, pending_state);
    step_done(update.get(), "complete idempotency claim");
    if (sqlite3_changes(db_) != 1)
      throw std::runtime_error("idempotency claim disappeared before completion");
    tx.commit();
  }

  void erase(const std::string& key, const std::string& fingerprint) override {
    std::scoped_lock lock(mutex_);
    transaction tx(*this);
    statement erase(db_, "DELETE FROM wuwe_runtime_idempotency WHERE key = ? AND fingerprint = ?");
    bind_text(erase.get(), 1, key);
    bind_text(erase.get(), 2, fingerprint);
    step_done(erase.get(), "erase idempotency claim");
    tx.commit();
  }

private:
  static constexpr int pending_state = 0;
  static constexpr int completed_state = 1;

  class statement {
  public:
    statement(sqlite3* db, const char* sql) : db_(db) {
      if (sqlite3_prepare_v2(db_, sql, -1, &statement_, nullptr) != SQLITE_OK)
        throw std::runtime_error(
          "prepare SQLite idempotency statement failed: " + std::string(sqlite3_errmsg(db_)));
    }

    ~statement() {
      if (statement_)
        sqlite3_finalize(statement_);
    }

    statement(const statement&) = delete;
    statement& operator=(const statement&) = delete;

    [[nodiscard]] sqlite3_stmt* get() const noexcept {
      return statement_;
    }

  private:
    sqlite3* db_ {};
    sqlite3_stmt* statement_ {};
  };

  class transaction {
  public:
    explicit transaction(sqlite_idempotency_repository& repository) : repository_(repository) {
      repository_.exec("BEGIN IMMEDIATE");
    }

    ~transaction() {
      if (active_) {
        try {
          repository_.exec("ROLLBACK");
        }
        catch (...) {
        }
      }
    }

    void commit() {
      repository_.exec("COMMIT");
      active_ = false;
    }

    void rollback() {
      repository_.exec("ROLLBACK");
      active_ = false;
    }

  private:
    sqlite_idempotency_repository& repository_;
    bool active_ { true };
  };

  void open() {
    if (path_.has_parent_path())
      std::filesystem::create_directories(path_.parent_path());
    const auto path_utf8 = agent::core::filesystem_path_to_utf8(path_);
    if (sqlite3_open(path_utf8.c_str(), &db_) != SQLITE_OK) {
      const std::string message = db_ ? sqlite3_errmsg(db_) : "unknown SQLite error";
      close();
      throw std::runtime_error("failed to open SQLite idempotency repository: " + message);
    }
    exec("PRAGMA foreign_keys = ON");
    exec("PRAGMA busy_timeout = 5000");
    exec("PRAGMA journal_mode = WAL");
  }

  void close() noexcept {
    if (db_) {
      sqlite3_close(db_);
      db_ = nullptr;
    }
  }

  void initialize_schema() {
    transaction tx(*this);
    exec("CREATE TABLE IF NOT EXISTS wuwe_runtime_schema ("
         "component TEXT PRIMARY KEY, version INTEGER NOT NULL)");
    agent::core::validate_sqlite_table(db_,
      "wuwe_runtime_schema",
      {
        { "component", "TEXT", false, 1 },
        { "version", "INTEGER", true, 0 },
      },
      "SQLite Runtime Host repository");
    auto version = schema_version().value_or(0);
    if (version > latest_schema_version) {
      throw std::runtime_error("SQLite Runtime Host idempotency schema version " +
                               std::to_string(version) + " is newer than supported version " +
                               std::to_string(latest_schema_version));
    }
    if (version == 0) {
      migrate_0_to_1();
      set_schema_version(1);
      version = 1;
    }
    if (version != latest_schema_version)
      throw std::runtime_error("unsupported SQLite Runtime Host idempotency schema version");
    validate_schema();
    tx.commit();
  }

  void migrate_0_to_1() {
    exec("CREATE TABLE IF NOT EXISTS wuwe_runtime_idempotency ("
         "key TEXT PRIMARY KEY,"
         "fingerprint TEXT NOT NULL,"
         "state INTEGER NOT NULL,"
         "result_json TEXT,"
         "created_at_ms INTEGER NOT NULL,"
         "updated_at_ms INTEGER NOT NULL)");
    exec("CREATE INDEX IF NOT EXISTS wuwe_runtime_idempotency_completed "
         "ON wuwe_runtime_idempotency(state, updated_at_ms)");
  }

  void validate_schema() const {
    agent::core::validate_sqlite_table(db_,
      "wuwe_runtime_idempotency",
      {
        { "key", "TEXT", false, 1 },
        { "fingerprint", "TEXT", true, 0 },
        { "state", "INTEGER", true, 0 },
        { "result_json", "TEXT", false, 0 },
        { "created_at_ms", "INTEGER", true, 0 },
        { "updated_at_ms", "INTEGER", true, 0 },
      },
      "SQLite Runtime Host idempotency repository");
  }

  [[nodiscard]] std::optional<int> schema_version() const {
    statement select(db_,
      "SELECT version FROM wuwe_runtime_schema WHERE component = 'idempotency'");
    const auto rc = sqlite3_step(select.get());
    if (rc == SQLITE_DONE)
      return std::nullopt;
    if (rc != SQLITE_ROW)
      throw_sqlite("read idempotency schema version");
    return sqlite3_column_int(select.get(), 0);
  }

  void set_schema_version(int version) {
    statement update(db_,
      "INSERT INTO wuwe_runtime_schema (component, version) VALUES ('idempotency', ?) "
      "ON CONFLICT(component) DO UPDATE SET version = excluded.version");
    sqlite3_bind_int(update.get(), 1, version);
    step_done(update.get(), "update idempotency schema version");
  }

  [[nodiscard]] std::size_t record_count() const {
    statement count(db_, "SELECT COUNT(*) FROM wuwe_runtime_idempotency");
    if (sqlite3_step(count.get()) != SQLITE_ROW)
      throw_sqlite("count idempotency records");
    return static_cast<std::size_t>(sqlite3_column_int64(count.get(), 0));
  }

  void prune_completed() {
    while (record_count() >= capacity_) {
      statement oldest(db_,
        "SELECT key FROM wuwe_runtime_idempotency WHERE state = ? "
        "ORDER BY updated_at_ms ASC LIMIT 1");
      sqlite3_bind_int(oldest.get(), 1, completed_state);
      const auto rc = sqlite3_step(oldest.get());
      if (rc == SQLITE_DONE)
        return;
      if (rc != SQLITE_ROW)
        throw_sqlite("select oldest idempotency record");
      const auto key = column_text(oldest.get(), 0);
      statement erase(db_, "DELETE FROM wuwe_runtime_idempotency WHERE key = ? AND state = ?");
      bind_text(erase.get(), 1, key);
      sqlite3_bind_int(erase.get(), 2, completed_state);
      step_done(erase.get(), "prune idempotency record");
    }
  }

  void exec(const char* sql) const {
    char* error {};
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &error) != SQLITE_OK) {
      const std::string message = error ? error : "unknown SQLite error";
      sqlite3_free(error);
      throw std::runtime_error("SQLite idempotency repository failed: " + message);
    }
  }

  void throw_sqlite(const char* operation) const {
    throw std::runtime_error(std::string(operation) + " failed: " + sqlite3_errmsg(db_));
  }

  void step_done(sqlite3_stmt* value, const char* operation) const {
    if (sqlite3_step(value) != SQLITE_DONE)
      throw_sqlite(operation);
  }

  static void bind_text(sqlite3_stmt* value, int index, const std::string& text) {
    sqlite3_bind_text(value, index, text.c_str(), static_cast<int>(text.size()), SQLITE_TRANSIENT);
  }

  static std::string column_text(sqlite3_stmt* value, int column) {
    const auto* text = sqlite3_column_text(value, column);
    return text ? reinterpret_cast<const char*>(text) : std::string {};
  }

  std::filesystem::path path_;
  const std::size_t capacity_;
  sqlite3* db_ {};
  std::mutex mutex_;
};

#endif

} // namespace

std::shared_ptr<idempotency_repository> make_memory_idempotency_repository(
  std::size_t capacity) {
  return std::make_shared<memory_idempotency_repository>(capacity);
}

std::shared_ptr<idempotency_repository> make_sqlite_idempotency_repository(
  const std::filesystem::path& path, std::size_t capacity) {
#if WUWE_HAS_SQLITE
  return std::make_shared<sqlite_idempotency_repository>(path, capacity);
#else
  (void)path;
  (void)capacity;
  throw std::runtime_error("SQLite idempotency storage requires WUWE_HAS_SQLITE=1");
#endif
}

} // namespace wuwe::runtime_host::detail
