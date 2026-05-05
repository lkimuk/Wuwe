#ifndef WUWE_AGENT_KNOWLEDGE_PARSER_REGISTRY_HPP
#define WUWE_AGENT_KNOWLEDGE_PARSER_REGISTRY_HPP

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <wuwe/agent/knowledge/file_knowledge_loader.hpp>
#include <wuwe/agent/knowledge/knowledge_record.hpp>
#include <wuwe/agent/knowledge/tika_knowledge_loader.hpp>

namespace wuwe::agent::knowledge {

class knowledge_document_parser {
public:
  virtual ~knowledge_document_parser() = default;

  virtual bool supports(const std::filesystem::path& path) const = 0;
  virtual knowledge_document parse(const std::filesystem::path& path) const = 0;
};

class file_knowledge_document_parser final : public knowledge_document_parser {
public:
  explicit file_knowledge_document_parser(
    std::vector<std::string> extensions = {
      ".csv", ".htm", ".html", ".json", ".md", ".markdown", ".rtf", ".txt"
    },
    file_knowledge_loader loader = {})
      : extensions_(std::move(extensions)), loader_(std::move(loader)) {
    normalize_extensions();
  }

  bool supports(const std::filesystem::path& path) const override {
    const auto extension = lowercase_extension(path);
    return std::find(extensions_.begin(), extensions_.end(), extension) != extensions_.end();
  }

  knowledge_document parse(const std::filesystem::path& path) const override {
    return loader_.load(path);
  }

private:
  void normalize_extensions() {
    for (auto& extension : extensions_) {
      std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
      });
    }
  }

  static std::string lowercase_extension(const std::filesystem::path& path) {
    auto extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
      return static_cast<char>(std::tolower(ch));
    });
    return extension;
  }

  std::vector<std::string> extensions_;
  file_knowledge_loader loader_;
};

class tika_knowledge_document_parser final : public knowledge_document_parser {
public:
  explicit tika_knowledge_document_parser(
    tika_knowledge_loader loader,
    std::vector<std::string> extensions = {
      ".pdf", ".doc", ".docx", ".ppt", ".pptx", ".xls", ".xlsx"
    })
      : loader_(std::move(loader)), extensions_(std::move(extensions)) {
    for (auto& extension : extensions_) {
      std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
      });
    }
  }

  bool supports(const std::filesystem::path& path) const override {
    auto extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
      return static_cast<char>(std::tolower(ch));
    });
    return std::find(extensions_.begin(), extensions_.end(), extension) != extensions_.end();
  }

  knowledge_document parse(const std::filesystem::path& path) const override {
    return loader_.load(path);
  }

private:
  tika_knowledge_loader loader_;
  std::vector<std::string> extensions_;
};

class knowledge_parser_registry {
public:
  void register_parser(std::shared_ptr<knowledge_document_parser> parser) {
    if (!parser) {
      throw std::invalid_argument("knowledge_parser_registry requires parser");
    }
    parsers_.push_back(std::move(parser));
  }

  const knowledge_document_parser* find_parser(const std::filesystem::path& path) const {
    for (const auto& parser : parsers_) {
      if (parser->supports(path)) {
        return parser.get();
      }
    }
    return nullptr;
  }

  knowledge_document parse(const std::filesystem::path& path) const {
    const auto* parser = find_parser(path);
    if (!parser) {
      throw std::runtime_error("no knowledge parser registered for: " + path.string());
    }
    return parser->parse(path);
  }

private:
  std::vector<std::shared_ptr<knowledge_document_parser>> parsers_;
};

} // namespace wuwe::agent::knowledge

#endif // WUWE_AGENT_KNOWLEDGE_PARSER_REGISTRY_HPP
