#ifndef WUWE_AGENT_KNOWLEDGE_URL_KNOWLEDGE_LOADER_HPP
#define WUWE_AGENT_KNOWLEDGE_URL_KNOWLEDGE_LOADER_HPP

#include <algorithm>
#include <cctype>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <wuwe/agent/knowledge/knowledge_hash.hpp>
#include <wuwe/agent/knowledge/knowledge_html.hpp>
#include <wuwe/agent/knowledge/knowledge_record.hpp>
#include <wuwe/net/default_http_client.h>
#include <wuwe/net/http_client.h>

namespace wuwe::agent::knowledge {

struct url_knowledge_loader_options {
  std::string id;
  std::string title;
  std::map<std::string, std::string> metadata;
  int timeout_ms { 300000 };
  bool extract_html_text { true };
};

class url_knowledge_loader {
public:
  explicit url_knowledge_loader(
    std::shared_ptr<::wuwe::http_client> http = std::make_shared<::wuwe::default_http_client>())
      : http_(std::move(http)) {
    if (!http_) {
      throw std::invalid_argument("url_knowledge_loader requires an http_client");
    }
  }

  knowledge_document load(
    const std::string& url,
    url_knowledge_loader_options options = {}) const {
    if (!is_supported_url(url)) {
      throw std::invalid_argument("knowledge URL must start with http:// or https://");
    }

    ::wuwe::http_request request {
      .method = "GET",
      .url = url,
      .headers = {
        { "Accept", "text/html,application/xhtml+xml,text/plain;q=0.9,*/*;q=0.8" },
      },
      .timeout = options.timeout_ms,
    };

    const auto response = http_->send(request);
    if (response.error_code) {
      throw std::runtime_error(
        "url knowledge loader failed to fetch URL: " + response.error_code.message());
    }

    const auto fetched = utf8_sanitizer::sanitize(response.body);
    const auto parsed_title = html_text_extractor::title(fetched);

    knowledge_document document;
    document.id = options.id.empty() ? default_id(url) : std::move(options.id);
    document.title = options.title.empty()
                       ? (parsed_title.empty() ? url : parsed_title)
                       : std::move(options.title);
    document.source_uri = url;
    document.content = options.extract_html_text
                         ? html_text_extractor::to_text(fetched, html_text_mode::structured)
                         : fetched;
    document.metadata = std::move(options.metadata);
    document.metadata.try_emplace("content_type", "text/html");
    document.metadata.try_emplace("extracted_as", options.extract_html_text ? "text" : "html");
    document.metadata.try_emplace("loader", "url");
    document.metadata["content_hash"] = stable_hash(document.content);
    return document;
  }

  static bool is_supported_url(std::string_view value) {
    return starts_with_case_insensitive(value, "http://") ||
           starts_with_case_insensitive(value, "https://");
  }

  static std::string default_id(std::string value) {
    for (auto& ch : value) {
      if (!std::isalnum(static_cast<unsigned char>(ch))) {
        ch = '-';
      }
    }
    while (!value.empty() && value.back() == '-') {
      value.pop_back();
    }
    return value.empty() ? "url-document" : value;
  }

private:
  static bool starts_with_case_insensitive(std::string_view text, std::string_view prefix) {
    if (text.size() < prefix.size()) {
      return false;
    }
    for (std::size_t index = 0; index < prefix.size(); ++index) {
      const auto lhs = static_cast<unsigned char>(text[index]);
      const auto rhs = static_cast<unsigned char>(prefix[index]);
      if (std::tolower(lhs) != std::tolower(rhs)) {
        return false;
      }
    }
    return true;
  }

  std::shared_ptr<::wuwe::http_client> http_;
};

} // namespace wuwe::agent::knowledge

#endif // WUWE_AGENT_KNOWLEDGE_URL_KNOWLEDGE_LOADER_HPP
