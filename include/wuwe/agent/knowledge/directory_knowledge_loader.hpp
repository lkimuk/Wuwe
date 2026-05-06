#ifndef WUWE_AGENT_KNOWLEDGE_DIRECTORY_KNOWLEDGE_LOADER_HPP
#define WUWE_AGENT_KNOWLEDGE_DIRECTORY_KNOWLEDGE_LOADER_HPP

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <wuwe/agent/knowledge/file_knowledge_loader.hpp>
#include <wuwe/agent/knowledge/knowledge_path.hpp>

namespace wuwe::agent::knowledge {

struct directory_knowledge_loader_options {
  bool recursive { true };
  std::set<std::string> extensions {
    ".csv", ".htm", ".html", ".json", ".md", ".markdown", ".rtf", ".txt"
  };
  std::map<std::string, std::string> metadata;
};

class directory_knowledge_loader {
public:
  explicit directory_knowledge_loader(file_knowledge_loader file_loader = {})
      : file_loader_(std::move(file_loader)) {
  }

  std::vector<knowledge_document> load(
    const std::filesystem::path& root,
    directory_knowledge_loader_options options = {}) const {
    if (!std::filesystem::exists(root)) {
      throw std::runtime_error("knowledge directory does not exist: " + root.string());
    }
    if (!std::filesystem::is_directory(root)) {
      throw std::runtime_error("knowledge path is not a directory: " + root.string());
    }

    std::vector<std::filesystem::path> paths;
    if (options.recursive) {
      for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        append_candidate(entry, options, paths);
      }
    }
    else {
      for (const auto& entry : std::filesystem::directory_iterator(root)) {
        append_candidate(entry, options, paths);
      }
    }

    std::sort(paths.begin(), paths.end());

    std::vector<knowledge_document> documents;
    documents.reserve(paths.size());
    for (const auto& path : paths) {
      auto metadata = options.metadata;
      metadata["relative_path"] = generic_path_to_utf8(std::filesystem::relative(path, root));

      auto id = metadata["relative_path"];
      for (auto& ch : id) {
        if (ch == '\\' || ch == '/' || ch == ' ' || ch == ':') {
          ch = '-';
        }
      }

      documents.push_back(file_loader_.load(path, {
        .id = std::move(id),
        .metadata = std::move(metadata),
      }));
    }
    return documents;
  }

private:
  static void append_candidate(
    const std::filesystem::directory_entry& entry,
    const directory_knowledge_loader_options& options,
    std::vector<std::filesystem::path>& paths) {
    if (!entry.is_regular_file()) {
      return;
    }

    auto extension = entry.path().extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
      return static_cast<char>(std::tolower(ch));
    });

    if (!options.extensions.empty() && !options.extensions.contains(extension)) {
      return;
    }
    paths.push_back(entry.path());
  }

  file_knowledge_loader file_loader_;
};

} // namespace wuwe::agent::knowledge

#endif // WUWE_AGENT_KNOWLEDGE_DIRECTORY_KNOWLEDGE_LOADER_HPP
