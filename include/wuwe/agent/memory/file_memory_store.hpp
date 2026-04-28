#ifndef WUWE_AGENT_MEMORY_FILE_MEMORY_STORE_HPP
#define WUWE_AGENT_MEMORY_FILE_MEMORY_STORE_HPP

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <wuwe/agent/memory/in_memory_store.hpp>

namespace wuwe::agent::memory {

namespace detail {

inline std::int64_t to_unix_millis(std::chrono::system_clock::time_point value) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(value.time_since_epoch()).count();
}

inline std::chrono::system_clock::time_point from_unix_millis(std::int64_t value) {
  return std::chrono::system_clock::time_point(std::chrono::milliseconds(value));
}

inline std::string memory_kind_to_storage(memory_kind kind) {
  return to_string(kind);
}

inline memory_kind memory_kind_from_storage(const std::string& value) {
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

inline memory_visibility memory_visibility_from_storage(const std::string& value) {
  if (value == "visible") {
    return memory_visibility::visible;
  }
  if (value == "hidden") {
    return memory_visibility::hidden;
  }
  throw std::invalid_argument("invalid memory visibility: " + value);
}

inline nlohmann::json scope_to_json(const memory_scope& scope) {
  return {
    { "tenant_id", scope.tenant_id },
    { "user_id", scope.user_id },
    { "application_id", scope.application_id },
    { "conversation_id", scope.conversation_id },
    { "agent_id", scope.agent_id },
  };
}

inline memory_scope scope_from_json(const nlohmann::json& json) {
  memory_scope scope;
  scope.tenant_id = json.value("tenant_id", "");
  scope.user_id = json.value("user_id", "");
  scope.application_id = json.value("application_id", "");
  scope.conversation_id = json.value("conversation_id", "");
  scope.agent_id = json.value("agent_id", "");
  return scope;
}

inline nlohmann::json record_to_json(const memory_record& record) {
  nlohmann::json json {
    { "id", record.id },
    { "kind", memory_kind_to_storage(record.kind) },
    { "visibility", to_string(record.visibility) },
    { "content", record.content },
    { "summary", record.summary },
    { "scope", scope_to_json(record.scope) },
    { "score", record.score },
    { "priority", record.priority },
    { "created_at", to_unix_millis(record.created_at) },
    { "updated_at", to_unix_millis(record.updated_at) },
    { "metadata", record.metadata },
  };

  if (record.expires_at) {
    json["expires_at"] = to_unix_millis(*record.expires_at);
  }
  else {
    json["expires_at"] = nullptr;
  }

  return json;
}

inline memory_record record_from_json(const nlohmann::json& json) {
  memory_record record;
  record.id = json.value("id", "");
  record.kind = memory_kind_from_storage(json.value("kind", "working"));
  record.visibility = memory_visibility_from_storage(json.value("visibility", "visible"));
  record.content = json.value("content", "");
  record.summary = json.value("summary", "");
  record.scope = scope_from_json(json.value("scope", nlohmann::json::object()));
  record.score = json.value("score", 0.0);
  record.priority = json.value("priority", 0);
  record.created_at = from_unix_millis(json.value("created_at", std::int64_t { 0 }));
  record.updated_at = from_unix_millis(json.value("updated_at", std::int64_t { 0 }));

  if (json.contains("expires_at") && !json["expires_at"].is_null()) {
    record.expires_at = from_unix_millis(json["expires_at"].get<std::int64_t>());
  }

  if (json.contains("metadata") && json["metadata"].is_object()) {
    record.metadata = json["metadata"].get<std::map<std::string, std::string>>();
  }

  return record;
}

} // namespace detail

class file_memory_store final : public memory_store {
public:
  explicit file_memory_store(std::filesystem::path path) : path_(std::move(path)) {
    load();
  }

  memory_record add(memory_record record) override {
    const auto now = std::chrono::system_clock::now();
    if (record.id.empty()) {
      record.id = "mem-" + std::to_string(++next_id_);
    }
    if (record.created_at == std::chrono::system_clock::time_point {}) {
      record.created_at = now;
    }
    record.updated_at = now;

    records_.push_back(record);
    save();
    return record;
  }

  std::optional<memory_record> get(
    const std::string& id, const memory_scope& scope) const override {
    const auto it = std::find_if(records_.begin(), records_.end(), [&](const memory_record& record) {
      return record.id == id && scope_matches(record.scope, scope);
    });

    if (it == records_.end()) {
      return std::nullopt;
    }

    return *it;
  }

  std::vector<memory_record> search(const memory_query& query) const override {
    std::vector<memory_record> result;

    for (auto record : records_) {
      if (!scope_matches(record.scope, query.scope)) {
        continue;
      }
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
    const auto it = std::find_if(records_.begin(), records_.end(), [&](const memory_record& current) {
      return current.id == record.id && scope_matches(current.scope, record.scope);
    });

    if (it == records_.end()) {
      return false;
    }

    record.created_at = it->created_at;
    record.updated_at = std::chrono::system_clock::now();
    *it = std::move(record);
    save();
    return true;
  }

  bool erase(const std::string& id, const memory_scope& scope) override {
    const auto old_size = records_.size();
    std::erase_if(records_, [&](const memory_record& record) {
      return record.id == id && scope_matches(record.scope, scope);
    });

    if (records_.size() == old_size) {
      return false;
    }

    save();
    return true;
  }

  std::size_t clear(const memory_scope& scope) override {
    const auto old_size = records_.size();
    std::erase_if(records_, [&](const memory_record& record) {
      return scope_matches(record.scope, scope);
    });

    const std::size_t erased = old_size - records_.size();
    if (erased != 0) {
      save();
    }
    return erased;
  }

private:
  void load() {
    records_.clear();
    if (!std::filesystem::exists(path_)) {
      return;
    }

    std::ifstream input(path_);
    if (!input) {
      throw std::runtime_error("failed to open memory file for reading: " + path_.string());
    }

    std::string line;
    while (std::getline(input, line)) {
      if (line.empty()) {
        continue;
      }

      auto record = detail::record_from_json(nlohmann::json::parse(line));
      if (record.id.rfind("mem-", 0) == 0) {
        const auto numeric = record.id.substr(4);
        try {
          next_id_ = (std::max)(next_id_, static_cast<std::size_t>(std::stoull(numeric)));
        }
        catch (...) {
        }
      }
      records_.push_back(std::move(record));
    }
  }

  void save() const {
    if (path_.has_parent_path()) {
      std::filesystem::create_directories(path_.parent_path());
    }

    const auto temp_path = path_.string() + ".tmp";
    {
      std::ofstream output(temp_path, std::ios::trunc);
      if (!output) {
        throw std::runtime_error("failed to open memory file for writing: " + temp_path);
      }

      for (const auto& record : records_) {
        output << detail::record_to_json(record).dump() << '\n';
      }
    }

    if (std::filesystem::exists(path_)) {
      std::filesystem::remove(path_);
    }
    std::filesystem::rename(temp_path, path_);
  }

private:
  std::filesystem::path path_;
  std::vector<memory_record> records_;
  std::size_t next_id_ { 0 };
};

} // namespace wuwe::agent::memory

#endif // WUWE_AGENT_MEMORY_FILE_MEMORY_STORE_HPP
