#ifndef WUWE_AGENT_KNOWLEDGE_DOCUMENT_ENRICHER_HPP
#define WUWE_AGENT_KNOWLEDGE_DOCUMENT_ENRICHER_HPP

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <wuwe/agent/core/message.hpp>
#include <wuwe/agent/knowledge/knowledge_record.hpp>
#include <wuwe/agent/llm/llm_client.h>

namespace wuwe::agent::knowledge {

class knowledge_document_enricher {
public:
  virtual ~knowledge_document_enricher() = default;

  virtual void enrich(knowledge_document& document) const = 0;
};

struct llm_knowledge_document_enricher_config {
  std::string model;
  double temperature {};
  std::size_t max_input_chars { 12000 };
  std::string metadata_key { "summary" };
  std::string system_prompt {
    "Summarize this document for retrieval. Focus on the main topics, named "
    "patterns, sections, and concepts. Return concise plain text."
  };
};

class llm_knowledge_document_enricher final : public knowledge_document_enricher {
public:
  llm_knowledge_document_enricher(
    std::shared_ptr<::wuwe::llm_client> client,
    llm_knowledge_document_enricher_config config = {})
      : client_(std::move(client)), config_(std::move(config)) {
    if (!client_) {
      throw std::invalid_argument("llm_knowledge_document_enricher requires llm_client");
    }
    if (config_.metadata_key.empty()) {
      throw std::invalid_argument("llm_knowledge_document_enricher requires metadata key");
    }
  }

  void enrich(knowledge_document& document) const override {
    if (document.content.empty()) {
      return;
    }

    auto input = document.content;
    if (input.size() > config_.max_input_chars) {
      input.resize(config_.max_input_chars);
    }

    auto request =
      ::wuwe::make_message()
      << ("system" < ::wuwe::says > config_.system_prompt)
      << ("user" < ::wuwe::says >
            ("Title: " + document.title + "\nSource: " + document.source_uri +
             "\n\nDocument excerpt:\n" + input));
    request.model = config_.model;
    request.temperature = config_.temperature;

    auto response = client_->complete(request);
    if (!response || response.content.empty()) {
      return;
    }

    document.metadata[config_.metadata_key] = std::move(response.content);
    if (!config_.model.empty()) {
      document.metadata["summary_model"] = config_.model;
    }
  }

private:
  std::shared_ptr<::wuwe::llm_client> client_;
  llm_knowledge_document_enricher_config config_;
};

} // namespace wuwe::agent::knowledge

#endif // WUWE_AGENT_KNOWLEDGE_DOCUMENT_ENRICHER_HPP
