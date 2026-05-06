#ifndef WUWE_AGENT_KNOWLEDGE_CONTEXT_HPP
#define WUWE_AGENT_KNOWLEDGE_CONTEXT_HPP

#include <algorithm>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include <wuwe/agent/knowledge/knowledge_retriever.hpp>
#include <wuwe/agent/knowledge/knowledge_result_processor.hpp>
#include <wuwe/agent/llm/llm_types.h>

namespace wuwe::agent::knowledge {

struct knowledge_policy {
  std::size_t max_context_chars { 8000 };
  std::size_t max_results { 6 };
  std::size_t candidate_results {};
  bool include_citations { true };
  bool inject_as_system_message { true };
  std::size_t surrounding_chunks_before {};
  std::size_t surrounding_chunks_after {};
  knowledge_access_scope access;
  knowledge_result_processing_policy result_processing {};
  std::string injection_header { "Relevant knowledge:" };
};

class knowledge_context {
public:
  explicit knowledge_context(
    std::shared_ptr<knowledge_retriever> retriever,
    knowledge_policy policy = {})
      : retriever_(std::move(retriever)), policy_(std::move(policy)) {
    if (!retriever_) {
      throw std::invalid_argument("knowledge_context requires a knowledge_retriever");
    }
  }

  std::string build_context_block(std::string_view query_text) const {
    knowledge_query query;
    query.text = std::string(query_text);
    query.limit = policy_.max_results;
    query.candidate_limit = policy_.candidate_results;
    query.access = policy_.access;

    std::ostringstream output;
    output << policy_.injection_header;

    std::size_t remaining = policy_.max_context_chars;
    std::size_t citation = 0;
    bool wrote_any = false;

    knowledge_result_processor processor(policy_.result_processing);
    auto results = retriever_->retrieve(query);
    results = retriever_->expand_with_neighbors(
      std::move(results),
      policy_.surrounding_chunks_before,
      policy_.surrounding_chunks_after,
      policy_.access);

    for (const auto& result : processor.process(std::move(results))) {
      std::string content = normalize(result.chunk.content);
      if (content.empty()) {
        continue;
      }

      std::ostringstream line;
      std::ostringstream prefix;
      if (policy_.include_citations) {
        prefix << "\n[" << ++citation << "] ";
        if (!result.chunk.source_uri.empty()) {
          prefix << result.chunk.source_uri;
        }
        else if (!result.chunk.title.empty()) {
          prefix << result.chunk.title;
        }
        else {
          prefix << result.chunk.document_id;
        }
        if (const auto section = result.chunk.metadata.find("section");
            section != result.chunk.metadata.end() && !section->second.empty()) {
          prefix << " | " << section->second;
        }
        if (const auto page = result.chunk.metadata.find("page_start");
            page != result.chunk.metadata.end() && !page->second.empty()) {
          prefix << " | page " << page->second;
          if (const auto page_end = result.chunk.metadata.find("page_end");
              page_end != result.chunk.metadata.end() && !page_end->second.empty() &&
              page_end->second != page->second) {
            prefix << "-" << page_end->second;
          }
        }
        if (result.chunk.start_line != 0) {
          prefix << " | lines " << result.chunk.start_line;
          if (result.chunk.end_line != 0 && result.chunk.end_line != result.chunk.start_line) {
            prefix << "-" << result.chunk.end_line;
          }
        }
        prefix << "\n";
      }
      else {
        prefix << "\n- ";
      }

      auto rendered_prefix = prefix.str();
      if (rendered_prefix.size() >= remaining) {
        continue;
      }

      const auto available_content = remaining - rendered_prefix.size();
      if (content.size() > available_content) {
        content.resize(available_content);
        if (content.size() > 3) {
          content.resize(content.size() - 3);
          content += "...";
        }
      }

      line << rendered_prefix << content;
      const auto rendered = line.str();
      output << rendered;
      remaining -= (std::min)(remaining, rendered.size());
      wrote_any = true;
      if (remaining == 0) {
        break;
      }
    }

    if (!wrote_any) {
      return {};
    }
    return output.str();
  }

  llm_request augment(llm_request request, std::string_view query_text) const {
    const auto block = build_context_block(query_text);
    if (block.empty()) {
      return request;
    }

    chat_message knowledge_message {
      .role = policy_.inject_as_system_message ? "system" : "user",
      .content = block,
    };

    if (!policy_.inject_as_system_message) {
      request.messages.insert(request.messages.begin(), std::move(knowledge_message));
      return request;
    }

    auto insert_at = request.messages.begin();
    while (insert_at != request.messages.end() && insert_at->role == "system") {
      ++insert_at;
    }
    request.messages.insert(insert_at, std::move(knowledge_message));
    return request;
  }

  const knowledge_policy& policy() const noexcept {
    return policy_;
  }

private:
  static std::string normalize(std::string text) {
    std::replace(text.begin(), text.end(), '\r', ' ');
    std::replace(text.begin(), text.end(), '\n', ' ');
    return text;
  }

  std::shared_ptr<knowledge_retriever> retriever_;
  knowledge_policy policy_;
};

} // namespace wuwe::agent::knowledge

#endif // WUWE_AGENT_KNOWLEDGE_CONTEXT_HPP
