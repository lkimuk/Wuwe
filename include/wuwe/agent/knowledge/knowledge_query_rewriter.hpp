#ifndef WUWE_AGENT_KNOWLEDGE_QUERY_REWRITER_HPP
#define WUWE_AGENT_KNOWLEDGE_QUERY_REWRITER_HPP

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <wuwe/net/default_http_client.h>
#include <wuwe/net/http_client.h>

namespace wuwe::agent::knowledge {

class knowledge_query_rewriter {
public:
  virtual ~knowledge_query_rewriter() = default;

  virtual std::vector<std::string> rewrite(const std::string& query) const = 0;
};

class static_knowledge_query_rewriter final : public knowledge_query_rewriter {
public:
  explicit static_knowledge_query_rewriter(std::vector<std::string> rewrites)
      : rewrites_(std::move(rewrites)) {
  }

  std::vector<std::string> rewrite(const std::string&) const override {
    return rewrites_;
  }

private:
  std::vector<std::string> rewrites_;
};

class callback_knowledge_query_rewriter final : public knowledge_query_rewriter {
public:
  using callback_type = std::function<std::vector<std::string>(const std::string&)>;

  explicit callback_knowledge_query_rewriter(callback_type callback)
      : callback_(std::move(callback)) {
  }

  std::vector<std::string> rewrite(const std::string& query) const override {
    return callback_ ? callback_(query) : std::vector<std::string> {};
  }

private:
  callback_type callback_;
};

struct http_knowledge_query_rewriter_config {
  std::string endpoint_url;
  std::string api_key;
  int timeout_ms { 30000 };
  std::size_t max_rewrites { 4 };
};

class http_knowledge_query_rewriter final : public knowledge_query_rewriter {
public:
  http_knowledge_query_rewriter(
    http_knowledge_query_rewriter_config config,
    std::shared_ptr<::wuwe::http_client> http = std::make_shared<::wuwe::default_http_client>())
      : config_(std::move(config)), http_(std::move(http)) {
    if (config_.endpoint_url.empty()) {
      throw std::invalid_argument("http_knowledge_query_rewriter requires endpoint_url");
    }
    if (!http_) {
      throw std::invalid_argument("http_knowledge_query_rewriter requires http_client");
    }
  }

  std::vector<std::string> rewrite(const std::string& query) const override {
    nlohmann::json body {
      { "query", query },
      { "max_rewrites", config_.max_rewrites },
    };

    ::wuwe::http_request request {
      .method = "POST",
      .url = config_.endpoint_url,
      .headers = { { "Content-Type", "application/json" } },
      .body = body.dump(),
      .timeout = config_.timeout_ms,
    };
    if (!config_.api_key.empty()) {
      request.headers.push_back({ "Authorization", "Bearer " + config_.api_key });
    }

    const auto response = http_->send(request);
    if (response.error_code) {
      throw std::runtime_error(
        "http knowledge query rewriter request failed: " + response.error_code.message());
    }

    const auto data = nlohmann::json::parse(response.body, nullptr, false);
    if (data.is_discarded()) {
      throw std::runtime_error("http knowledge query rewriter received invalid JSON");
    }

    const auto& items = data.is_array()
                          ? data
                          : data.contains("rewrites") ? data["rewrites"] : nlohmann::json::array();
    if (!items.is_array()) {
      throw std::runtime_error("http knowledge query rewriter expected rewrites array");
    }

    std::vector<std::string> rewrites;
    for (const auto& item : items) {
      if (!item.is_string()) {
        continue;
      }
      auto value = item.get<std::string>();
      if (!value.empty()) {
        rewrites.push_back(std::move(value));
      }
      if (rewrites.size() >= config_.max_rewrites) {
        break;
      }
    }
    return rewrites;
  }

private:
  http_knowledge_query_rewriter_config config_;
  std::shared_ptr<::wuwe::http_client> http_;
};

} // namespace wuwe::agent::knowledge

#endif // WUWE_AGENT_KNOWLEDGE_QUERY_REWRITER_HPP
