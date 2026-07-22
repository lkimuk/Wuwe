#ifndef WUWE_AGENT_KNOWLEDGE_DOCUMENT_LOADER_HPP
#define WUWE_AGENT_KNOWLEDGE_DOCUMENT_LOADER_HPP

#include <algorithm>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <wuwe/agent/knowledge/knowledge_parser_registry.hpp>
#include <wuwe/agent/knowledge/knowledge_path.hpp>
#include <wuwe/agent/knowledge/knowledge_document_enricher.hpp>
#include <wuwe/agent/knowledge/tika_runtime.hpp>
#include <wuwe/agent/knowledge/url_knowledge_loader.hpp>

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
  explicit knowledge_document_loader(
    knowledge_parser_registry registry,
    std::shared_ptr<url_knowledge_loader> url_loader = std::make_shared<url_knowledge_loader>(),
    std::vector<std::shared_ptr<tika_runtime_process>> runtimes = {})
      : registry_(std::move(registry)),
        url_loader_(std::move(url_loader)),
        runtimes_(std::move(runtimes)) {
  }

  static knowledge_document_loader make_default() {
    knowledge_parser_registry registry;
    registry.register_parser(std::make_shared<file_knowledge_document_parser>());
    std::vector<std::shared_ptr<tika_runtime_process>> runtimes;
    std::string tika_url;
    if (auto runtime = tika_runtime_process::ensure_running()) {
      tika_url = runtime->base_url();
      runtimes.push_back(std::move(runtime));
    }
    if (!tika_url.empty()) {
      registry.register_parser(
        std::make_shared<tika_knowledge_document_parser>(
          tika_knowledge_loader({
            .base_url = std::move(tika_url),
          })));
    }
    return knowledge_document_loader(std::move(registry),
      std::make_shared<url_knowledge_loader>(),
      std::move(runtimes));
  }

  std::vector<knowledge_document> load(
    std::string_view source,
    knowledge_document_load_options options = {}) const {
    const std::string source_text(source);
    if (url_knowledge_loader::is_supported_url(source_text)) {
      return load_url(source_text, std::move(options));
    }

    return load(path_from_utf8(source_text), std::move(options));
  }

  std::vector<knowledge_document> load(
    const std::string& source,
    knowledge_document_load_options options = {}) const {
    return load(std::string_view(source), std::move(options));
  }

  std::vector<knowledge_document> load(
    const char* source,
    knowledge_document_load_options options = {}) const {
    if (source == nullptr) {
      throw std::invalid_argument("knowledge source must not be null");
    }
    return load(std::string_view(source), std::move(options));
  }

  std::vector<knowledge_document> load(
    const std::filesystem::path& root,
    knowledge_document_load_options options = {}) const {
    const auto root_text = generic_path_to_utf8(root);
    if (url_knowledge_loader::is_supported_url(root_text)) {
      return load_url(root_text, std::move(options));
    }

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
  std::vector<knowledge_document> load_url(
    const std::string& url,
    knowledge_document_load_options options) const {
    if (!url_loader_) {
      throw std::runtime_error("knowledge document loader has no URL loader configured");
    }

    auto document = url_loader_->load(url, {
      .metadata = options.metadata,
    });
    for (const auto& enricher : options.enrichers) {
      if (enricher) {
        enricher->enrich(document);
      }
    }
    return { std::move(document) };
  }

  knowledge_parser_registry registry_;
  std::shared_ptr<url_knowledge_loader> url_loader_;
  std::vector<std::shared_ptr<tika_runtime_process>> runtimes_;
};

} // namespace wuwe::agent::knowledge

#endif // WUWE_AGENT_KNOWLEDGE_DOCUMENT_LOADER_HPP
