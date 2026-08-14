#ifndef WUWE_AGENT_TRAINING_TRAINING_STORE_HPP
#define WUWE_AGENT_TRAINING_TRAINING_STORE_HPP

#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include <wuwe/agent/core/storage.hpp>
#include <wuwe/agent/training/training_codec.hpp>
#include <wuwe/agent/training/training_core.hpp>
#include <wuwe/agent/training/training_error.hpp>
#include <wuwe/common/sha256.hpp>

namespace wuwe::agent::training {

[[nodiscard]] inline std::optional<std::uint64_t> next_training_revision(
  std::uint64_t revision) noexcept {
  if (revision == (std::numeric_limits<std::uint64_t>::max)())
    return std::nullopt;
  return revision + 1;
}

struct training_job_create_result {
  training_job job;
  bool inserted { false };
};

class training_job_store {
public:
  virtual ~training_job_store() = default;
  [[nodiscard]] virtual core::storage_capabilities capabilities() const noexcept {
    return {};
  }
  virtual training_result<training_job_create_result> create(training_job value) = 0;
  [[nodiscard]] virtual training_result<std::optional<training_job>> load(
    const std::string& id) const = 0;
  [[nodiscard]] virtual training_result<std::optional<training_job>> find_by_idempotency_key(
    const std::string& tenant_id, const std::string& workspace_id,
    const std::string& key) const = 0;
  [[nodiscard]] virtual training_result<std::vector<training_job>> list() const = 0;
  virtual training_result<training_job> update(
    training_job value, std::uint64_t expected_revision) = 0;
};

class in_memory_training_job_store final : public training_job_store {
public:
  [[nodiscard]] core::storage_capabilities capabilities() const noexcept override {
    return {
      .declared = true,
      .durable = false,
      .optimistic_concurrency = true,
      .atomic_mutations = true,
      .coordination_scope = core::storage_coordination_scope::process_local,
      .schema_version = 1,
    };
  }

  training_result<training_job_create_result> create(training_job value) override {
    try {
      validate_training_job(value);
    }
    catch (const std::exception& ex) {
      return invalid_create(ex.what());
    }
    if (value.id.empty() || value.request.idempotency_key.empty())
      return invalid_create("training job requires id and idempotency key");
    std::scoped_lock lock(mutex_);
    const auto key = scope_key(value);
    const auto existing = idempotency_index_.find(key);
    if (existing != idempotency_index_.end()) {
      const auto& job = jobs_.at(existing->second);
      if (job.request.id != value.request.id ||
          training_request_digest(job.request) != training_request_digest(value.request)) {
        return invalid_create("training idempotency key already exists for a different request");
      }
      return training_result<training_job_create_result>::success({ .job = job });
    }
    if (jobs_.contains(value.id))
      return invalid_create("training job id already exists: " + value.id);
    value.revision = 1;
    value.updated_at = (std::max)(std::chrono::system_clock::now(), value.created_at);
    jobs_.emplace(value.id, value);
    idempotency_index_.emplace(key, value.id);
    return training_result<training_job_create_result>::success(
      { .job = std::move(value), .inserted = true });
  }

  [[nodiscard]] training_result<std::optional<training_job>> load(
    const std::string& id) const override {
    std::scoped_lock lock(mutex_);
    const auto found = jobs_.find(id);
    return training_result<std::optional<training_job>>::success(
      found == jobs_.end() ? std::nullopt : std::optional(found->second));
  }

  [[nodiscard]] training_result<std::optional<training_job>> find_by_idempotency_key(
    const std::string& tenant_id, const std::string& workspace_id,
    const std::string& key) const override {
    std::scoped_lock lock(mutex_);
    const auto found = idempotency_index_.find(scope_key(tenant_id, workspace_id, key));
    return training_result<std::optional<training_job>>::success(
      found == idempotency_index_.end() ? std::nullopt : std::optional(jobs_.at(found->second)));
  }

  [[nodiscard]] training_result<std::vector<training_job>> list() const override {
    std::scoped_lock lock(mutex_);
    std::vector<training_job> output;
    output.reserve(jobs_.size());
    for (const auto& [_, value] : jobs_)
      output.push_back(value);
    return training_result<std::vector<training_job>>::success(std::move(output));
  }

  training_result<training_job> update(
    training_job value, std::uint64_t expected_revision) override {
    std::scoped_lock lock(mutex_);
    const auto found = jobs_.find(value.id);
    if (found == jobs_.end())
      return training_result<training_job>::failure(
        { .code = training_errc::not_found, .message = "training job not found: " + value.id });
    if (found->second.revision != expected_revision)
      return training_result<training_job>::failure({ .code = training_errc::revision_conflict,
        .message = "training job revision conflict: " + value.id,
        .retryable = true });
    const auto next_revision = next_training_revision(expected_revision);
    if (!next_revision)
      return training_result<training_job>::failure({ .code = training_errc::revision_conflict,
        .message = "training job revision limit reached: " + value.id });
    try {
      validate_training_job_update(found->second, value);
    }
    catch (const std::exception& ex) {
      return invalid_update(ex.what());
    }
    value.revision = *next_revision;
    value.created_at = found->second.created_at;
    value.updated_at = (std::max)(std::chrono::system_clock::now(), found->second.updated_at);
    found->second = value;
    return training_result<training_job>::success(std::move(value));
  }

private:
  using idempotency_scope_key = std::tuple<std::string, std::string, std::string>;

  static idempotency_scope_key scope_key(const training_job& value) {
    return scope_key(value.tenant_id, value.workspace_id, value.request.idempotency_key);
  }

  static idempotency_scope_key scope_key(
    const std::string& tenant_id, const std::string& workspace_id, const std::string& key) {
    return { tenant_id, workspace_id, key };
  }

  static training_result<training_job_create_result> invalid_create(std::string message) {
    return training_result<training_job_create_result>::failure(
      { .code = training_errc::invalid_request, .message = std::move(message) });
  }

  static training_result<training_job> invalid_update(std::string message) {
    return training_result<training_job>::failure(
      { .code = training_errc::invalid_request, .message = std::move(message) });
  }

  mutable std::mutex mutex_;
  std::map<std::string, training_job> jobs_;
  std::map<idempotency_scope_key, std::string> idempotency_index_;
};

class file_training_job_store final : public training_job_store {
public:
  explicit file_training_job_store(std::filesystem::path directory)
      : directory_(std::move(directory)) {
    if (directory_.empty())
      throw std::invalid_argument("file training job store requires a directory");
    std::filesystem::create_directories(directory_);
    reload();
  }

  [[nodiscard]] core::storage_capabilities capabilities() const noexcept override {
    return {
      .declared = true,
      .durable = false,
      .optimistic_concurrency = true,
      .atomic_mutations = false,
      .ordered_replay = true,
      .coordination_scope = core::storage_coordination_scope::process_local,
      .schema_version = 1,
    };
  }

  training_result<training_job_create_result> create(training_job value) override {
    try {
      validate_training_job(value);
    }
    catch (const std::exception& ex) {
      return invalid_create(ex.what());
    }
    if (value.id.empty() || value.request.idempotency_key.empty())
      return invalid_create("training job requires id and idempotency key");
    std::scoped_lock lock(mutex_);
    const auto duplicate = idempotency_index_.find(scope_key(value));
    if (duplicate != idempotency_index_.end()) {
      const auto& existing = jobs_.at(duplicate->second);
      if (existing.request.id != value.request.id ||
          training_request_digest(existing.request) != training_request_digest(value.request)) {
        return invalid_create("training idempotency key already exists for a different request");
      }
      return training_result<training_job_create_result>::success({ .job = existing });
    }
    if (jobs_.contains(value.id))
      return invalid_create("training job id already exists: " + value.id);
    value.revision = 1;
    value.updated_at = (std::max)(std::chrono::system_clock::now(), value.created_at);
    try {
      write_revision(value);
    }
    catch (const std::exception& ex) {
      return persistence_create(ex.what());
    }
    jobs_[value.id] = value;
    idempotency_index_[scope_key(value)] = value.id;
    return training_result<training_job_create_result>::success(
      { .job = std::move(value), .inserted = true });
  }

  [[nodiscard]] training_result<std::optional<training_job>> load(
    const std::string& id) const override {
    std::scoped_lock lock(mutex_);
    const auto found = jobs_.find(id);
    return training_result<std::optional<training_job>>::success(
      found == jobs_.end() ? std::nullopt : std::optional(found->second));
  }

  [[nodiscard]] training_result<std::optional<training_job>> find_by_idempotency_key(
    const std::string& tenant_id, const std::string& workspace_id,
    const std::string& key) const override {
    std::scoped_lock lock(mutex_);
    const auto found = idempotency_index_.find(scope_key(tenant_id, workspace_id, key));
    return training_result<std::optional<training_job>>::success(
      found == idempotency_index_.end() ? std::nullopt : std::optional(jobs_.at(found->second)));
  }

  [[nodiscard]] training_result<std::vector<training_job>> list() const override {
    std::scoped_lock lock(mutex_);
    std::vector<training_job> output;
    output.reserve(jobs_.size());
    for (const auto& [_, value] : jobs_)
      output.push_back(value);
    return training_result<std::vector<training_job>>::success(std::move(output));
  }

  training_result<training_job> update(
    training_job value, std::uint64_t expected_revision) override {
    std::scoped_lock lock(mutex_);
    const auto found = jobs_.find(value.id);
    if (found == jobs_.end())
      return training_result<training_job>::failure(
        { .code = training_errc::not_found, .message = "training job not found: " + value.id });
    if (found->second.revision != expected_revision)
      return training_result<training_job>::failure({ .code = training_errc::revision_conflict,
        .message = "training job revision conflict: " + value.id,
        .retryable = true });
    const auto next_revision = next_training_revision(expected_revision);
    if (!next_revision)
      return training_result<training_job>::failure({ .code = training_errc::revision_conflict,
        .message = "training job revision limit reached: " + value.id });
    try {
      validate_training_job_update(found->second, value);
    }
    catch (const std::exception& ex) {
      return invalid_update(ex.what());
    }
    value.revision = *next_revision;
    value.created_at = found->second.created_at;
    value.updated_at = (std::max)(std::chrono::system_clock::now(), found->second.updated_at);
    try {
      write_revision(value);
    }
    catch (const std::exception& ex) {
      return persistence_update(ex.what());
    }
    found->second = value;
    return training_result<training_job>::success(std::move(value));
  }

private:
  using idempotency_scope_key = std::tuple<std::string, std::string, std::string>;

  static idempotency_scope_key scope_key(const training_job& value) {
    return scope_key(value.tenant_id, value.workspace_id, value.request.idempotency_key);
  }

  static idempotency_scope_key scope_key(
    const std::string& tenant_id, const std::string& workspace_id, const std::string& key) {
    return { tenant_id, workspace_id, key };
  }

  static training_result<training_job_create_result> invalid_create(std::string message) {
    return training_result<training_job_create_result>::failure(
      { .code = training_errc::invalid_request, .message = std::move(message) });
  }

  static training_result<training_job> invalid_update(std::string message) {
    return training_result<training_job>::failure(
      { .code = training_errc::invalid_request, .message = std::move(message) });
  }

  static training_result<training_job_create_result> persistence_create(std::string message) {
    return training_result<training_job_create_result>::failure(
      { .code = training_errc::persistence_failure,
        .message = std::move(message),
        .retryable = true });
  }

  static training_result<training_job> persistence_update(std::string message) {
    return training_result<training_job>::failure({ .code = training_errc::persistence_failure,
      .message = std::move(message),
      .retryable = true });
  }

  static std::string key_for(const std::string& id) {
    return common::sha256_hex(id);
  }

  std::filesystem::path revision_path(const std::string& id, std::uint64_t revision) const {
    return directory_ / (key_for(id) + "." + std::to_string(revision) + ".json");
  }

  void write_revision(const training_job& value) const {
    const auto destination = revision_path(value.id, value.revision);
    if (std::filesystem::exists(destination))
      throw std::runtime_error("training job revision already exists: " + destination.string());
    const auto temporary = destination.string() + ".tmp." + make_training_id("write");
    try {
      {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output)
          throw std::runtime_error("failed to open training job revision: " + temporary);
        output << training_job_record_to_json(value).dump(2);
        output.flush();
        if (!output)
          throw std::runtime_error("failed to write training job revision: " + temporary);
      }
      std::filesystem::rename(temporary, destination);
    }
    catch (...) {
      std::error_code ignored;
      std::filesystem::remove(temporary, ignored);
      throw;
    }
  }

  void reload() {
    std::scoped_lock lock(mutex_);
    jobs_.clear();
    idempotency_index_.clear();
    std::map<std::string, std::map<std::uint64_t, training_job>> revisions;
    for (const auto& entry : std::filesystem::directory_iterator(directory_)) {
      if (!entry.is_regular_file() || entry.path().extension() != ".json")
        continue;
      std::ifstream input(entry.path(), std::ios::binary);
      if (!input)
        throw std::runtime_error("failed to read training job revision: " + entry.path().string());
      nlohmann::json encoded;
      input >> encoded;
      auto job = training_job_record_from_json(encoded);
      if (entry.path() != revision_path(job.id, job.revision)) {
        throw std::runtime_error(
          "training job revision filename does not match its contents: " + entry.path().string());
      }
      if (!revisions[job.id].emplace(job.revision, std::move(job)).second)
        throw std::runtime_error("duplicate training job revision: " + entry.path().string());
    }
    for (auto& [id, history] : revisions) {
      std::uint64_t expected_revision = 1;
      std::optional<training_job> previous;
      for (auto& [revision, job] : history) {
        if (revision != expected_revision++) {
          throw std::runtime_error("training job revision history is not contiguous: " + id);
        }
        validate_training_job(job);
        if (previous)
          validate_training_job_update(*previous, job);
        previous = job;
      }
      jobs_[id] = std::move(*previous);
    }
    for (const auto& [id, job] : jobs_) {
      const auto [found, inserted] = idempotency_index_.emplace(scope_key(job), id);
      if (!inserted && found->second != id) {
        throw std::runtime_error(
          "training store contains duplicate idempotency keys for different jobs");
      }
    }
  }

  std::filesystem::path directory_;
  mutable std::mutex mutex_;
  std::map<std::string, training_job> jobs_;
  std::map<idempotency_scope_key, std::string> idempotency_index_;
};

} // namespace wuwe::agent::training

#endif // WUWE_AGENT_TRAINING_TRAINING_STORE_HPP
