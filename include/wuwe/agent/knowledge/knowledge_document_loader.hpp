#ifndef WUWE_AGENT_KNOWLEDGE_DOCUMENT_LOADER_HPP
#define WUWE_AGENT_KNOWLEDGE_DOCUMENT_LOADER_HPP

#include <algorithm>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <wuwe/agent/knowledge/knowledge_parser_registry.hpp>
#include <wuwe/agent/knowledge/knowledge_path.hpp>
#include <wuwe/agent/knowledge/knowledge_document_enricher.hpp>

namespace wuwe::agent::knowledge {

struct knowledge_document_load_options {
  std::map<std::string, std::string> metadata;
  std::vector<std::shared_ptr<knowledge_document_enricher>> enrichers;
};

inline std::string sanitize_document_id(std::string value) {
  for (auto& ch : value) {
    if (ch == '\\' || ch == '/' || ch == ' ' || ch == ':') {
      ch = '-';
    }
  }
  return value;
}

class knowledge_document_loader {
public:
  explicit knowledge_document_loader(knowledge_parser_registry registry)
      : registry_(std::move(registry)) {
  }

  static knowledge_document_loader make_default(std::string tika_url = {}) {
    knowledge_parser_registry registry;
    registry.register_parser(std::make_shared<file_knowledge_document_parser>());
    if (!tika_url.empty()) {
      registry.register_parser(
        std::make_shared<tika_knowledge_document_parser>(
          tika_knowledge_loader({
            .base_url = std::move(tika_url),
          })));
    }
    return knowledge_document_loader(std::move(registry));
  }

  std::vector<knowledge_document> load(
    const std::filesystem::path& root,
    knowledge_document_load_options options = {}) const {
    if (!std::filesystem::exists(root)) {
      throw std::runtime_error("knowledge path does not exist: " + path_to_utf8(root));
    }

    std::vector<std::filesystem::path> paths;
    if (std::filesystem::is_regular_file(root)) {
      if (registry_.find_parser(root)) {
        paths.push_back(root);
      }
    }
    else {
      for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (entry.is_regular_file() && registry_.find_parser(entry.path())) {
          paths.push_back(entry.path());
        }
      }
    }
    std::sort(paths.begin(), paths.end());

    std::vector<knowledge_document> documents;
    documents.reserve(paths.size());
    for (const auto& path : paths) {
      auto document = registry_.parse(path);
      const auto relative_path =
        std::filesystem::is_directory(root)
          ? generic_path_to_utf8(std::filesystem::relative(path, root))
          : filename_to_utf8(path);

      document.id = sanitize_document_id(relative_path);
      document.source_uri = relative_path;
      document.metadata["relative_path"] = relative_path;
      for (const auto& [key, value] : options.metadata) {
        document.metadata[key] = value;
      }
      for (const auto& enricher : options.enrichers) {
        if (enricher) {
          enricher->enrich(document);
        }
      }
      documents.push_back(std::move(document));
    }
    return documents;
  }

private:
  knowledge_parser_registry registry_;
};

} // namespace wuwe::agent::knowledge

#endif // WUWE_AGENT_KNOWLEDGE_DOCUMENT_LOADER_HPP
