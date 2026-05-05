#ifndef WUWE_AGENT_KNOWLEDGE_FILE_KNOWLEDGE_STORE_HPP
#define WUWE_AGENT_KNOWLEDGE_FILE_KNOWLEDGE_STORE_HPP

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <wuwe/agent/knowledge/knowledge_store.hpp>

namespace wuwe::agent::knowledge {

namespace detail {

inline nlohmann::json knowledge_document_to_json(const knowledge_document& document) {
  return {
    { "id", document.id },
    { "title", document.title },
    { "content", document.content },
    { "source_uri", document.source_uri },
    { "metadata", document.metadata },
  };
}

inline knowledge_document knowledge_document_from_json(const nlohmann::json& json) {
  knowledge_document document;
  document.id = json.value("id", "");
  document.title = json.value("title", "");
  document.content = json.value("content", "");
  document.source_uri = json.value("source_uri", "");
  if (json.contains("metadata") && json["metadata"].is_object()) {
    document.metadata = json["metadata"].get<std::map<std::string, std::string>>();
  }
  return document;
}

inline nlohmann::json knowledge_chunk_to_json(const knowledge_chunk& chunk) {
  return {
    { "id", chunk.id },
    { "document_id", chunk.document_id },
    { "title", chunk.title },
    { "content", chunk.content },
    { "start_offset", chunk.start_offset },
    { "end_offset", chunk.end_offset },
    { "start_line", chunk.start_line },
    { "end_line", chunk.end_line },
    { "source_uri", chunk.source_uri },
    { "metadata", chunk.metadata },
  };
}

inline knowledge_chunk knowledge_chunk_from_json(const nlohmann::json& json) {
  knowledge_chunk chunk;
  chunk.id = json.value("id", "");
  chunk.document_id = json.value("document_id", "");
  chunk.title = json.value("title", "");
  chunk.content = json.value("content", "");
  chunk.start_offset = json.value("start_offset", std::size_t {});
  chunk.end_offset = json.value("end_offset", std::size_t {});
  chunk.start_line = json.value("start_line", std::size_t {});
  chunk.end_line = json.value("end_line", std::size_t {});
  chunk.source_uri = json.value("source_uri", "");
  if (json.contains("metadata") && json["metadata"].is_object()) {
    chunk.metadata = json["metadata"].get<std::map<std::string, std::string>>();
  }
  return chunk;
}

} // namespace detail

class file_knowledge_store final : public knowledge_store {
public:
  explicit file_knowledge_store(std::filesystem::path path) : path_(std::move(path)) {
    load();
  }

  void add_document(knowledge_document document) override {
    std::scoped_lock lock(mutex_);
    documents_[document.id] = std::move(document);
    rewrite();
  }

  std::vector<knowledge_document> list_documents() const override {
    std::scoped_lock lock(mutex_);

    std::vector<knowledge_document> result;
    result.reserve(documents_.size());
    for (const auto& [_, document] : documents_) {
      result.push_back(document);
    }
    std::sort(result.begin(), result.end(), [](const knowledge_document& lhs,
                                                const knowledge_document& rhs) {
      return lhs.id < rhs.id;
    });
    return result;
  }

  void add_chunks(std::vector<knowledge_chunk> chunks) override {
    std::scoped_lock lock(mutex_);
    for (auto& chunk : chunks) {
      chunks_[chunk.id] = std::move(chunk);
    }
    rewrite();
  }

  std::vector<knowledge_chunk> list_chunks(const knowledge_query& query) const override {
    std::scoped_lock lock(mutex_);

    std::vector<knowledge_chunk> result;
    for (const auto& [_, chunk] : chunks_) {
      if (!metadata_matches(chunk.metadata, query.filters) ||
          !metadata_access_matches(chunk.metadata, query.access)) {
        continue;
      }
      result.push_back(chunk);
    }

    std::sort(result.begin(), result.end(), [](const knowledge_chunk& lhs,
                                                const knowledge_chunk& rhs) {
      if (lhs.document_id != rhs.document_id) {
        return lhs.document_id < rhs.document_id;
      }
      return lhs.start_offset < rhs.start_offset;
    });

    if (query.limit != 0 && result.size() > query.limit) {
      result.resize(query.limit);
    }
    return result;
  }

  bool erase_document(const std::string& document_id) override {
    std::scoped_lock lock(mutex_);
    const bool erased_document = documents_.erase(document_id) != 0;
    const auto old_size = chunks_.size();

    for (auto it = chunks_.begin(); it != chunks_.end();) {
      if (it->second.document_id == document_id) {
        it = chunks_.erase(it);
      }
      else {
        ++it;
      }
    }

    const bool erased = erased_document || chunks_.size() != old_size;
    if (erased) {
      rewrite();
    }
    return erased;
  }

  void clear() override {
    std::scoped_lock lock(mutex_);
    documents_.clear();
    chunks_.clear();
    rewrite();
  }

private:
  void load() {
    documents_.clear();
    chunks_.clear();

    if (!std::filesystem::exists(path_)) {
      return;
    }

    std::ifstream input(path_);
    if (!input) {
      throw std::runtime_error("failed to open knowledge store for reading: " + path_.string());
    }

    std::string line;
    while (std::getline(input, line)) {
      if (line.empty()) {
        continue;
      }

      const auto json = nlohmann::json::parse(line);
      const auto kind = json.value("kind", "");
      if (kind == "document") {
        auto document = detail::knowledge_document_from_json(json.at("value"));
        documents_[document.id] = std::move(document);
      }
      else if (kind == "chunk") {
        auto chunk = detail::knowledge_chunk_from_json(json.at("value"));
        chunks_[chunk.id] = std::move(chunk);
      }
    }
  }

  void ensure_parent_directory() const {
    if (path_.has_parent_path()) {
      std::filesystem::create_directories(path_.parent_path());
    }
  }

  void rewrite() const {
    ensure_parent_directory();

    const auto temp_path = path_.string() + ".tmp";
    {
      std::ofstream output(temp_path, std::ios::trunc);
      if (!output) {
        throw std::runtime_error("failed to open knowledge store for writing: " + temp_path);
      }

      for (const auto& [_, document] : documents_) {
        output << nlohmann::json {
          { "kind", "document" },
          { "value", detail::knowledge_document_to_json(document) },
        }.dump() << '\n';
      }
      for (const auto& [_, chunk] : chunks_) {
        output << nlohmann::json {
          { "kind", "chunk" },
          { "value", detail::knowledge_chunk_to_json(chunk) },
        }.dump() << '\n';
      }
    }

    if (std::filesystem::exists(path_)) {
      std::filesystem::remove(path_);
    }
    std::filesystem::rename(temp_path, path_);
  }

  std::filesystem::path path_;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, knowledge_document> documents_;
  std::unordered_map<std::string, knowledge_chunk> chunks_;
};

} // namespace wuwe::agent::knowledge

#endif // WUWE_AGENT_KNOWLEDGE_FILE_KNOWLEDGE_STORE_HPP
