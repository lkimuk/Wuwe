#ifndef WUWE_AGENT_KNOWLEDGE_TIKA_KNOWLEDGE_LOADER_HPP
#define WUWE_AGENT_KNOWLEDGE_TIKA_KNOWLEDGE_LOADER_HPP

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

#include <wuwe/agent/knowledge/file_knowledge_loader.hpp>
#include <wuwe/agent/knowledge/knowledge_hash.hpp>
#include <wuwe/agent/knowledge/knowledge_record.hpp>
#include <wuwe/net/default_http_client.h>
#include <wuwe/net/http_client.h>

namespace wuwe::agent::knowledge {

struct tika_knowledge_loader_config {
  std::string base_url { "http://localhost:9998" };
  std::string endpoint { "/tika" };
  int timeout_ms { 30000 };
};

struct tika_knowledge_loader_options {
  std::string id;
  std::string title;
  std::string source_uri;
  std::string content_type;
  std::map<std::string, std::string> metadata;
};

class tika_knowledge_loader {
public:
  explicit tika_knowledge_loader(
    tika_knowledge_loader_config config = {},
    std::shared_ptr<::wuwe::http_client> http = std::make_shared<::wuwe::default_http_client>())
      : config_(std::move(config)), http_(std::move(http)) {
    if (!http_) {
      throw std::invalid_argument("tika_knowledge_loader requires an http_client");
    }
    if (config_.base_url.empty()) {
      throw std::invalid_argument("tika_knowledge_loader requires base_url");
    }
    if (config_.endpoint.empty() || config_.endpoint.front() != '/') {
      throw std::invalid_argument("tika_knowledge_loader endpoint must start with '/'");
    }
  }

  knowledge_document load(
    const std::filesystem::path& path,
    tika_knowledge_loader_options options = {}) const {
    const auto body = read_file(path);
    const auto extension = lowercase_extension(path);
    const auto content_type = options.content_type.empty()
                                ? default_content_type(extension)
                                : std::move(options.content_type);

    ::wuwe::http_request request {
      .method = "PUT",
      .url = endpoint(),
      .headers = {
        { "Accept", "text/plain" },
        { "Content-Type", content_type },
      },
      .body = body,
      .timeout = config_.timeout_ms,
    };

    const auto response = http_->send(request);
    if (response.error_code) {
      throw std::runtime_error(
        "tika knowledge loader failed to parse file: " + response.error_code.message());
    }

    knowledge_document document;
    document.id = options.id.empty()
                    ? file_knowledge_loader::default_id(path)
                    : std::move(options.id);
    document.title = options.title.empty() ? path.stem().string() : std::move(options.title);
    document.content = response.body;
    document.source_uri =
      options.source_uri.empty() ? path.generic_string() : std::move(options.source_uri);
    document.metadata = std::move(options.metadata);
    if (!extension.empty()) {
      document.metadata.try_emplace("extension", extension);
    }
    document.metadata.try_emplace("content_type", content_type);
    document.metadata.try_emplace("parser", "tika");
    document.metadata.try_emplace("extracted_as", "text");
    document.metadata["content_hash"] = stable_hash(document.content);
    return document;
  }

private:
  static std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
      throw std::runtime_error("failed to open Tika knowledge file: " + path.string());
    }
    std::ostringstream content;
    content << input.rdbuf();
    return content.str();
  }

  static std::string lowercase_extension(const std::filesystem::path& path) {
    auto extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
      return static_cast<char>(std::tolower(ch));
    });
    return extension;
  }

  static std::string default_content_type(const std::string& extension) {
    if (extension == ".pdf") {
      return "application/pdf";
    }
    if (extension == ".docx") {
      return "application/vnd.openxmlformats-officedocument.wordprocessingml.document";
    }
    if (extension == ".pptx") {
      return "application/vnd.openxmlformats-officedocument.presentationml.presentation";
    }
    if (extension == ".xlsx") {
      return "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet";
    }
    if (extension == ".doc") {
      return "application/msword";
    }
    if (extension == ".ppt") {
      return "application/vnd.ms-powerpoint";
    }
    if (extension == ".xls") {
      return "application/vnd.ms-excel";
    }
    if (extension == ".rtf") {
      return "application/rtf";
    }
    return "application/octet-stream";
  }

  std::string endpoint() const {
    auto base_url = config_.base_url;
    while (!base_url.empty() && base_url.back() == '/') {
      base_url.pop_back();
    }
    return base_url + config_.endpoint;
  }

  tika_knowledge_loader_config config_;
  std::shared_ptr<::wuwe::http_client> http_;
};

} // namespace wuwe::agent::knowledge

#endif // WUWE_AGENT_KNOWLEDGE_TIKA_KNOWLEDGE_LOADER_HPP
