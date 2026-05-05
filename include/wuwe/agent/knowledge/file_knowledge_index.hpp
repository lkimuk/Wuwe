#ifndef WUWE_AGENT_KNOWLEDGE_FILE_KNOWLEDGE_INDEX_HPP
#define WUWE_AGENT_KNOWLEDGE_FILE_KNOWLEDGE_INDEX_HPP

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

#include <wuwe/agent/knowledge/file_knowledge_store.hpp>
#include <wuwe/agent/knowledge/in_memory_knowledge_index.hpp>

namespace wuwe::agent::knowledge {

namespace detail {

inline nlohmann::json knowledge_embedding_to_json(const std::vector<float>& embedding) {
  auto json = nlohmann::json::array();
  for (const auto value : embedding) {
    json.push_back(value);
  }
  return json;
}

inline std::vector<float> knowledge_embedding_from_json(const nlohmann::json& json) {
  std::vector<float> embedding;
  if (!json.is_array()) {
    return embedding;
  }
  embedding.reserve(json.size());
  for (const auto& value : json) {
    embedding.push_back(value.get<float>());
  }
  return embedding;
}

} // namespace detail

class file_knowledge_index final : public knowledge_index {
public:
  explicit file_knowledge_index(std::filesystem::path path) : path_(std::move(path)) {
    load();
  }

  void upsert(const knowledge_chunk& chunk, const std::vector<float>& embedding) override {
    std::scoped_lock lock(mutex_);
    entries_[chunk.id] = entry {
      .chunk = chunk,
      .embedding = embedding,
    };
    rewrite();
  }

  void upsert_batch(
    const std::vector<knowledge_chunk>& chunks,
    const std::vector<std::vector<float>>& embeddings) override {
    if (chunks.size() != embeddings.size()) {
      throw std::invalid_argument("file_knowledge_index upsert_batch size mismatch");
    }

    std::scoped_lock lock(mutex_);
    for (std::size_t index = 0; index < chunks.size(); ++index) {
      entries_[chunks[index].id] = entry {
        .chunk = chunks[index],
        .embedding = embeddings[index],
      };
    }
    rewrite();
  }

  std::vector<knowledge_result> search(
    const knowledge_query& query,
    const std::vector<float>& embedding) const override {
    std::scoped_lock lock(mutex_);

    std::vector<knowledge_result> result;
    for (const auto& [_, item] : entries_) {
      if (!metadata_matches(item.chunk.metadata, query.filters) ||
          !metadata_access_matches(item.chunk.metadata, query.access)) {
        continue;
      }

      const auto vector_score =
        ::wuwe::agent::memory::vector_detail::cosine_similarity(embedding, item.embedding);
      const auto lexical_score = detail::lexical_knowledge_score(query.text, item.chunk);
      const auto score =
        query.vector_weight * vector_score + query.lexical_weight * lexical_score;
      if (score < query.minimum_score) {
        continue;
      }

      result.push_back({
        .chunk = item.chunk,
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
    const auto old_size = entries_.size();

    for (auto it = entries_.begin(); it != entries_.end();) {
      if (it->second.chunk.document_id == document_id) {
        it = entries_.erase(it);
      }
      else {
        ++it;
      }
    }

    const bool erased = old_size != entries_.size();
    if (erased) {
      rewrite();
    }
    return erased;
  }

  void clear() override {
    std::scoped_lock lock(mutex_);
    entries_.clear();
    rewrite();
  }

private:
  struct entry {
    knowledge_chunk chunk;
    std::vector<float> embedding;
  };

  static nlohmann::json entry_to_json(const entry& item) {
    return {
      { "chunk", detail::knowledge_chunk_to_json(item.chunk) },
      { "embedding", detail::knowledge_embedding_to_json(item.embedding) },
    };
  }

  static entry entry_from_json(const nlohmann::json& json) {
    return {
      .chunk = detail::knowledge_chunk_from_json(json.at("chunk")),
      .embedding = detail::knowledge_embedding_from_json(json.at("embedding")),
    };
  }

  void load() {
    entries_.clear();
    if (!std::filesystem::exists(path_)) {
      return;
    }

    std::ifstream input(path_);
    if (!input) {
      throw std::runtime_error("failed to open knowledge index for reading: " + path_.string());
    }

    std::string line;
    while (std::getline(input, line)) {
      if (line.empty()) {
        continue;
      }

      auto item = entry_from_json(nlohmann::json::parse(line));
      entries_[item.chunk.id] = std::move(item);
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
        throw std::runtime_error("failed to open knowledge index for writing: " + temp_path);
      }

      for (const auto& [_, item] : entries_) {
        output << entry_to_json(item).dump() << '\n';
      }
    }

    if (std::filesystem::exists(path_)) {
      std::filesystem::remove(path_);
    }
    std::filesystem::rename(temp_path, path_);
  }

  std::filesystem::path path_;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, entry> entries_;
};

} // namespace wuwe::agent::knowledge

#endif // WUWE_AGENT_KNOWLEDGE_FILE_KNOWLEDGE_INDEX_HPP
