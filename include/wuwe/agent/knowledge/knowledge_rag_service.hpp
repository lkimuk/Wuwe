#ifndef WUWE_AGENT_KNOWLEDGE_RAG_SERVICE_HPP
#define WUWE_AGENT_KNOWLEDGE_RAG_SERVICE_HPP

#include <chrono>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <wuwe/agent/core/message.hpp>
#include <wuwe/agent/knowledge/knowledge_context.hpp>
#include <wuwe/agent/knowledge/knowledge_document_loader.hpp>
#include <wuwe/agent/knowledge/knowledge_retriever.hpp>
#include <wuwe/agent/llm/llm_client.h>
#include <wuwe/agent/llm/llm_types.h>

namespace wuwe::agent::knowledge {

struct knowledge_ingestion_stage {
  std::string name;
  double elapsed_ms {};
};

struct knowledge_upload_report {
  std::size_t documents {};
  knowledge_ingest_result ingest;
  double load_ms {};
  double ingest_ms {};
  double total_ms {};
  std::vector<knowledge_ingestion_stage> stages;
};

struct knowledge_answer_request {
  std::string query;
  std::string model;
  knowledge_policy policy;
  bool generate_answer { true };
};

struct knowledge_answer_report {
  std::string context_block;
  std::vector<knowledge_result> citations;
  knowledge_retrieval_trace trace;
  llm_response answer;
  double retrieve_ms {};
  double context_ms {};
  double answer_ms {};
  double total_ms {};
};

class knowledge_rag_service {
public:
  knowledge_rag_service(
    std::shared_ptr<knowledge_retriever> retriever,
    knowledge_document_loader loader,
    std::shared_ptr<::wuwe::llm_client> llm = {})
      : retriever_(std::move(retriever)),
        loader_(std::move(loader)),
        llm_(std::move(llm)) {
    if (!retriever_) {
      throw std::invalid_argument("knowledge_rag_service requires knowledge_retriever");
    }
  }

  knowledge_upload_report upload_document(
    const std::filesystem::path& path,
    knowledge_document_load_options options = {},
    bool erase_stale = false) {
    const auto total_start = clock::now();

    const auto load_start = clock::now();
    auto documents = loader_.load(path, std::move(options));
    knowledge_upload_report report;
    report.documents = documents.size();
    report.load_ms = elapsed_ms(load_start);
    report.stages.push_back({ .name = "load", .elapsed_ms = report.load_ms });

    const auto ingest_start = clock::now();
    report.ingest = retriever_->ingest_incremental(documents, erase_stale);
    report.ingest_ms = elapsed_ms(ingest_start);
    report.stages.push_back({ .name = "ingest", .elapsed_ms = report.ingest_ms });
    report.total_ms = elapsed_ms(total_start);
    return report;
  }

  knowledge_answer_report ask(knowledge_answer_request request) const {
    if (request.query.empty()) {
      throw std::invalid_argument("knowledge_rag_service ask requires query");
    }

    const auto total_start = clock::now();
    knowledge_answer_report report;

    knowledge_query query;
    query.text = request.query;
    query.limit = request.policy.max_results;
    query.candidate_limit = request.policy.candidate_results;
    query.access = request.policy.access;

    const auto retrieve_start = clock::now();
    auto retrieval = retriever_->retrieve_detailed(query);
    report.retrieve_ms = elapsed_ms(retrieve_start);
    report.trace = retrieval.trace;
    report.citations = std::move(retrieval.results);

    const auto context_start = clock::now();
    knowledge_context context(retriever_, request.policy);
    report.context_block = context.build_context_block(request.query);
    report.context_ms = elapsed_ms(context_start);

    if (request.generate_answer && llm_ && !report.context_block.empty()) {
      const auto answer_start = clock::now();
      auto llm_request =
        ::wuwe::make_message()
        << ("system" < ::wuwe::says >
              "Answer using only the provided knowledge. Include citation numbers like [1] "
              "for every factual claim. If the knowledge is insufficient, say so.")
        << ("user" < ::wuwe::says > request.query);
      llm_request = context.augment(std::move(llm_request), request.query);
      llm_request.model = request.model;
      llm_request.temperature = 0.1;
      report.answer = llm_->complete(llm_request);
      report.answer_ms = elapsed_ms(answer_start);
    }

    report.total_ms = elapsed_ms(total_start);
    return report;
  }

private:
  using clock = std::chrono::steady_clock;

  static double elapsed_ms(clock::time_point start) {
    return static_cast<double>(
      std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - start).count()) /
           1000.0;
  }

  std::shared_ptr<knowledge_retriever> retriever_;
  knowledge_document_loader loader_;
  std::shared_ptr<::wuwe::llm_client> llm_;
};

} // namespace wuwe::agent::knowledge

#endif // WUWE_AGENT_KNOWLEDGE_RAG_SERVICE_HPP
