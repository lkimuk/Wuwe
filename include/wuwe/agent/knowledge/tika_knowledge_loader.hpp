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
#include <wuwe/agent/knowledge/knowledge_path.hpp>
#include <wuwe/agent/knowledge/knowledge_record.hpp>
#include <wuwe/agent/knowledge/knowledge_text.hpp>
#include <wuwe/net/default_http_client.h>
#include <wuwe/net/http_client.h>

namespace wuwe::agent::knowledge {

struct tika_knowledge_loader_config {
  std::string base_url { "http://localhost:9998" };
  std::string endpoint { "/tika" };
  int timeout_ms { 30000 };
  bool extract_pdf_pages { true };
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

    std::string content = sanitize_utf8(response.body);
    std::map<std::string, std::string> parsed_metadata;
    if (extension == ".pdf" && config_.extract_pdf_pages) {
      const auto paged = parse_pdf_pages(body, content_type);
      if (!paged.content.empty()) {
        content = std::move(paged.content);
        parsed_metadata["page_count"] = std::to_string(paged.page_count);
        parsed_metadata["page_extraction"] = "tika-html";
      }
    }

    knowledge_document document;
    document.id = options.id.empty()
                    ? file_knowledge_loader::default_id(path)
                    : std::move(options.id);
    document.title = options.title.empty() ? stem_to_utf8(path) : std::move(options.title);
    document.content = std::move(content);
    document.source_uri =
      options.source_uri.empty() ? generic_path_to_utf8(path) : std::move(options.source_uri);
    document.metadata = std::move(options.metadata);
    for (auto& [key, value] : parsed_metadata) {
      document.metadata[key] = std::move(value);
    }
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
  struct paged_content {
    std::string content;
    std::size_t page_count {};
  };

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

  paged_content parse_pdf_pages(
    const std::string& body,
    const std::string& content_type) const {
    ::wuwe::http_request request {
      .method = "PUT",
      .url = endpoint(),
      .headers = {
        { "Accept", "text/html" },
        { "Content-Type", content_type },
      },
      .body = body,
      .timeout = config_.timeout_ms,
    };

    const auto response = http_->send(request);
    if (response.error_code || response.body.empty()) {
      return {};
    }
    return extract_pages_from_html(response.body);
  }

  static paged_content extract_pages_from_html(const std::string& html) {
    std::vector<std::string> pages;
    std::size_t search = 0;
    while (search < html.size()) {
      const auto div_start = text_detail::find_case_insensitive(html, "<div", search);
      if (div_start == std::string::npos) {
        break;
      }
      const auto tag_end = html.find('>', div_start);
      if (tag_end == std::string::npos) {
        break;
      }
      const auto tag = html.substr(div_start, tag_end - div_start + 1);
      const auto lower_tag = text_detail::lowercase_ascii(tag);
      if (lower_tag.find("page") == std::string::npos) {
        search = tag_end + 1;
        continue;
      }
      const auto div_end = text_detail::find_case_insensitive(html, "</div>", tag_end + 1);
      if (div_end == std::string::npos) {
        break;
      }
      auto text = html_to_text(html.substr(tag_end + 1, div_end - tag_end - 1));
      if (!text_detail::trim_copy(text).empty()) {
        pages.push_back(std::move(text));
      }
      search = div_end + 6;
    }

    if (pages.size() <= 1) {
      return {};
    }

    std::ostringstream output;
    for (std::size_t index = 0; index < pages.size(); ++index) {
      if (index != 0) {
        output << '\f';
      }
      output << pages[index];
    }
    return {
      .content = sanitize_utf8(output.str()),
      .page_count = pages.size(),
    };
  }

  static std::string html_to_text(std::string html) {
    html = text_detail::decode_common_html_entities(html);

    std::ostringstream output;
    bool previous_space = false;
    for (std::size_t index = 0; index < html.size(); ++index) {
      const auto ch = html[index];
      if (ch == '<') {
        const auto tag_end = html.find('>', index);
        if (tag_end == std::string::npos) {
          break;
        }
        const auto tag = text_detail::lowercase_ascii(html.substr(index, tag_end - index + 1));
        if (tag.find("<br") == 0 || tag.find("</p") == 0 || tag.find("</div") == 0) {
          output << '\n';
          previous_space = false;
        }
        else if (!previous_space) {
          output << ' ';
          previous_space = true;
        }
        index = tag_end;
        continue;
      }
      if (ch == '\r') {
        continue;
      }
      if (ch == '\n') {
        output << '\n';
        previous_space = false;
        continue;
      }
      if (std::isspace(static_cast<unsigned char>(ch))) {
        if (!previous_space) {
          output << ' ';
          previous_space = true;
        }
        continue;
      }
      output << ch;
      previous_space = false;
    }
    return output.str();
  }

  static std::string sanitize_utf8(const std::string& text) {
    std::string output;
    output.reserve(text.size());

    for (std::size_t index = 0; index < text.size();) {
      const auto byte = static_cast<unsigned char>(text[index]);
      if (byte < 0x80) {
        output.push_back(text[index++]);
        continue;
      }

      std::size_t length = 0;
      if ((byte & 0xE0) == 0xC0) {
        length = 2;
      }
      else if ((byte & 0xF0) == 0xE0) {
        length = 3;
      }
      else if ((byte & 0xF8) == 0xF0) {
        length = 4;
      }
      else {
        output.push_back('?');
        ++index;
        continue;
      }

      if (index + length > text.size()) {
        output.push_back('?');
        break;
      }

      bool valid = true;
      for (std::size_t offset = 1; offset < length; ++offset) {
        const auto continuation = static_cast<unsigned char>(text[index + offset]);
        if ((continuation & 0xC0) != 0x80) {
          valid = false;
          break;
        }
      }

      if (!valid) {
        output.push_back('?');
        ++index;
        continue;
      }

      output.append(text, index, length);
      index += length;
    }

    return output;
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
