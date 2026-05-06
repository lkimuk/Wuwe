#ifndef WUWE_AGENT_KNOWLEDGE_RETRIEVER_HPP
#define WUWE_AGENT_KNOWLEDGE_RETRIEVER_HPP

#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include <wuwe/agent/knowledge/knowledge_index.hpp>
#include <wuwe/agent/knowledge/knowledge_cache.hpp>
#include <wuwe/agent/knowledge/knowledge_observability.hpp>
#include <wuwe/agent/knowledge/knowledge_query_rewriter.hpp>
#include <wuwe/agent/knowledge/knowledge_reranker.hpp>
#include <wuwe/agent/knowledge/knowledge_splitter.hpp>
#include <wuwe/agent/knowledge/knowledge_store.hpp>
#include <wuwe/agent/knowledge/knowledge_text.hpp>
#include <wuwe/agent/memory/embedding_model.hpp>

namespace wuwe::agent::knowledge {

struct knowledge_ingest_result {
  std::size_t ingested {};
  std::size_t skipped {};
  std::size_t erased_stale {};
  std::vector<std::string> errors;
};

struct knowledge_rebuild_result {
  std::size_t scanned {};
  std::size_t rebuilt {};
  std::size_t skipped {};
  std::vector<std::string> errors;
};

struct knowledge_indexing_policy {
  std::string embedding_provider;
  std::string embedding_model;
  std::string embedding_version;
  std::size_t expected_embedding_dimension {};
  std::size_t index_schema_version { 1 };
  std::size_t embedding_batch_size { 64 };
};

struct knowledge_retrieval_trace {
  std::string trace_id;
  std::string query_text;
  std::size_t requested_limit {};
  std::size_t candidate_limit {};
  std::size_t first_stage_count {};
  std::size_t after_access_filter_count {};
  std::size_t final_count {};
  std::size_t rewritten_query_count {};
  bool cache_hit {};
  bool used_reranker {};
  double embedding_ms {};
  double index_search_ms {};
  double access_filter_ms {};
  double rerank_ms {};
  double total_ms {};
};

struct knowledge_retrieval_report {
  std::vector<knowledge_result> results;
  knowledge_retrieval_trace trace;
};

enum class knowledge_task_state {
  pending,
  running,
  completed,
  failed,
  canceled,
};

struct knowledge_task_progress {
  knowledge_task_state state { knowledge_task_state::pending };
  std::size_t completed {};
  std::size_t total {};
  std::string message;
  std::vector<std::string> errors;
};

using knowledge_task_progress_callback =
  std::function<void(const knowledge_task_progress&)>;

struct knowledge_task_policy {
  std::size_t max_retries {};
  std::chrono::milliseconds retry_backoff { 50 };
};

template<typename Result>
class knowledge_task {
public:
  explicit knowledge_task(std::future<Result> future) : future_(std::move(future)) {
  }

  Result get() {
    return future_.get();
  }

  bool ready() const {
    return future_.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
  }

  knowledge_task_progress progress() const {
    std::scoped_lock lock(mutex_);
    return progress_;
  }

  void update_progress(knowledge_task_progress progress) {
    std::scoped_lock lock(mutex_);
    progress_ = std::move(progress);
  }

  void request_cancel() {
    cancel_requested_.store(true);
  }

  bool cancel_requested() const {
    return cancel_requested_.load();
  }

private:
  std::future<Result> future_;
  mutable std::mutex mutex_;
  knowledge_task_progress progress_;
  std::atomic<bool> cancel_requested_ { false };
};

class knowledge_retriever {
public:
  knowledge_retriever(
    std::shared_ptr<knowledge_store> store,
    std::shared_ptr<knowledge_index> index,
    std::shared_ptr<::wuwe::agent::memory::embedding_model> embedding_model,
    knowledge_splitter splitter = knowledge_splitter(),
    knowledge_indexing_policy indexing_policy = {})
      : store_(std::move(store)),
        index_(std::move(index)),
        embedding_model_(std::move(embedding_model)),
        splitter_(std::move(splitter)),
        indexing_policy_(std::move(indexing_policy)) {
    if (!store_) {
      throw std::invalid_argument("knowledge_retriever requires a knowledge_store");
    }
    if (!index_) {
      throw std::invalid_argument("knowledge_retriever requires a knowledge_index");
    }
    if (!embedding_model_) {
      throw std::invalid_argument("knowledge_retriever requires an embedding_model");
    }
  }

  std::vector<knowledge_chunk> ingest(knowledge_document document) {
    const auto trace_id = next_trace_id();
    if (document.id.empty()) {
      document.id = "doc-" + std::to_string(++next_document_id_);
    }
    const auto document_id = document.id;
    publish_event(trace_id, "knowledge.ingest.start", {
      { "document_id", document_id },
    });

    try {
      auto chunks = splitter_.split(document);
      std::vector<std::string> texts;
      texts.reserve(chunks.size());
      for (const auto& chunk : chunks) {
        texts.push_back(chunk.content);
      }

      auto embeddings = embed_texts_in_batches(texts);
      if (embeddings.size() != chunks.size()) {
        throw std::runtime_error("knowledge embedding batch size mismatch");
      }
      for (std::size_t index = 0; index < chunks.size(); ++index) {
        apply_indexing_metadata(chunks[index], embeddings[index]);
      }

      store_->add_document(std::move(document));
      store_->add_chunks(chunks);

      if (!chunks.empty()) {
        index_->upsert_batch(chunks, embeddings);
      }
      publish_event(trace_id, "knowledge.ingest.complete", {
        { "document_id", document_id },
        { "chunks", std::to_string(chunks.size()) },
      });
      clear_retrieval_cache();
      return chunks;
    }
    catch (const std::exception& ex) {
      publish_event(trace_id, "knowledge.ingest.failed", {
        { "document_id", document_id },
        { "error", ex.what() },
      });
      throw;
    }
  }

  knowledge_ingest_result ingest_batch(const std::vector<knowledge_document>& documents) {
    knowledge_ingest_result result;
    for (const auto& document : documents) {
      try {
        ingest(document);
        ++result.ingested;
      }
      catch (const std::exception& ex) {
        result.errors.push_back(document.id + ": " + ex.what());
      }
    }
    return result;
  }

  std::shared_ptr<knowledge_task<knowledge_ingest_result>> ingest_batch_async(
    std::vector<knowledge_document> documents,
    knowledge_task_progress_callback progress_callback = {},
    knowledge_task_policy task_policy = {}) {
    auto promise = std::make_shared<std::promise<knowledge_ingest_result>>();
    auto task = std::make_shared<knowledge_task<knowledge_ingest_result>>(promise->get_future());

    task->update_progress({
      .state = knowledge_task_state::pending,
      .completed = 0,
      .total = documents.size(),
      .message = "ingest pending",
    });

    std::thread(
      [this, task, promise, documents = std::move(documents),
        progress_callback = std::move(progress_callback),
        task_policy]() mutable {
        knowledge_ingest_result result;
        auto publish = [&](knowledge_task_progress progress) {
          progress.errors = result.errors;
          task->update_progress(progress);
          if (progress_callback) {
            progress_callback(progress);
          }
        };

        try {
          publish({
            .state = knowledge_task_state::running,
            .completed = 0,
            .total = documents.size(),
            .message = "ingest running",
          });

          for (std::size_t index = 0; index < documents.size(); ++index) {
            if (task->cancel_requested()) {
              publish({
                .state = knowledge_task_state::canceled,
                .completed = index,
                .total = documents.size(),
                .message = "ingest canceled",
              });
              promise->set_value(std::move(result));
              return;
            }

            bool ingested = false;
            for (std::size_t attempt = 0; attempt <= task_policy.max_retries; ++attempt) {
              try {
                ingest(documents[index]);
                ++result.ingested;
                ingested = true;
                break;
              }
              catch (const std::exception& ex) {
                if (attempt == task_policy.max_retries) {
                  result.errors.push_back(documents[index].id + ": " + ex.what());
                }
                else {
                  std::this_thread::sleep_for(task_policy.retry_backoff);
                }
              }
            }

            publish({
              .state = knowledge_task_state::running,
              .completed = index + 1,
              .total = documents.size(),
              .message = ingested ? "ingest running" : "ingest running with errors",
            });
          }

          publish({
            .state = knowledge_task_state::completed,
            .completed = documents.size(),
            .total = documents.size(),
            .message = "ingest completed",
          });
          promise->set_value(std::move(result));
        }
        catch (const std::exception& ex) {
          result.errors.push_back(std::string("ingest task failed: ") + ex.what());
          publish({
            .state = knowledge_task_state::failed,
            .completed = result.ingested,
            .total = documents.size(),
            .message = ex.what(),
          });
          promise->set_value(std::move(result));
        }
      }).detach();

    return task;
  }

  knowledge_ingest_result ingest_incremental(
    const std::vector<knowledge_document>& documents,
    bool erase_stale = false) {
    knowledge_ingest_result result;

    std::map<std::string, knowledge_document> existing;
    for (auto& document : store_->list_documents()) {
      existing[document.id] = std::move(document);
    }

    std::set<std::string> seen;
    for (const auto& document : documents) {
      seen.insert(document.id);

      const auto current = existing.find(document.id);
      if (current != existing.end() && same_content_hash(current->second, document)) {
        ++result.skipped;
        continue;
      }

      if (current != existing.end()) {
        erase_document(document.id);
      }

      try {
        ingest(document);
        ++result.ingested;
      }
      catch (const std::exception& ex) {
        result.errors.push_back(document.id + ": " + ex.what());
      }
    }

    if (erase_stale) {
      for (const auto& [id, _] : existing) {
        if (!seen.contains(id) && erase_document(id)) {
          ++result.erased_stale;
        }
      }
    }

    return result;
  }

  std::vector<knowledge_result> retrieve(knowledge_query query) const {
    return retrieve_detailed(std::move(query)).results;
  }

  knowledge_retrieval_report retrieve_detailed(knowledge_query query) const {
    knowledge_retrieval_report report;
    if (query.text.empty()) {
      return report;
    }
    if (query.limit == 0) {
      query.limit = 1;
    }
    if (query.candidate_limit == 0 && looks_like_broad_knowledge_query(query.text)) {
      query.candidate_limit = (std::max)(query.limit * 4, std::size_t { 12 });
    }

    const auto trace_id = next_trace_id();
    publish_event(trace_id, "knowledge.retrieve.start", {
      { "query", query.text },
      { "limit", std::to_string(query.limit) },
      { "candidate_limit", std::to_string(query.candidate_limit) },
    });

    const auto total_start = clock::now();
    report.trace.trace_id = trace_id;
    report.trace.query_text = query.text;
    report.trace.requested_limit = query.limit;
    report.trace.candidate_limit = query.candidate_limit;
      report.trace.used_reranker = static_cast<bool>(reranker_);

    try {
      const auto cache_key = knowledge_query_cache_key(query);
      if (retrieval_cache_) {
        std::vector<knowledge_result> cached_results;
        if (retrieval_cache_->get(cache_key, cached_results)) {
          report.trace.cache_hit = true;
          report.trace.final_count = cached_results.size();
          report.trace.total_ms = elapsed_ms(total_start);
          report.results = std::move(cached_results);
          publish_event(trace_id, "knowledge.retrieve.cache_hit", {
            { "final_count", std::to_string(report.trace.final_count) },
            { "total_ms", std::to_string(report.trace.total_ms) },
          });
          return report;
        }
      }

      std::vector<std::string> query_texts { query.text };
      if (query_rewriter_) {
        for (auto& rewritten : query_rewriter_->rewrite(query.text)) {
          if (!rewritten.empty() &&
              std::find(query_texts.begin(), query_texts.end(), rewritten) == query_texts.end()) {
            query_texts.push_back(std::move(rewritten));
          }
        }
      }
      report.trace.rewritten_query_count = query_texts.size() - 1;

      auto index_query = query;
      if (query.candidate_limit > query.limit) {
        index_query.limit = query.candidate_limit;
      }

      std::vector<knowledge_result> results;
      for (const auto& query_text : query_texts) {
        auto expanded_query = index_query;
        expanded_query.text = query_text;

        const auto embedding_start = clock::now();
        auto embedding = embedding_model_->embed(query_text);
        report.trace.embedding_ms += elapsed_ms(embedding_start);

        const auto search_start = clock::now();
        auto query_results = index_->search(expanded_query, embedding);
        report.trace.index_search_ms += elapsed_ms(search_start);
        report.trace.first_stage_count += query_results.size();
        merge_results(results, std::move(query_results));
      }

      const auto access_filter_start = clock::now();
      if (access_filter_) {
        std::erase_if(results, [&](const knowledge_result& result) {
          return !access_filter_(result.chunk);
        });
      }
      report.trace.access_filter_ms = elapsed_ms(access_filter_start);
      report.trace.after_access_filter_count = results.size();
      apply_document_summary_boost(query.text, results);

      if (reranker_) {
        const auto rerank_start = clock::now();
        results = reranker_->rerank(query, std::move(results));
        report.trace.rerank_ms = elapsed_ms(rerank_start);
      }
      else if (query.limit != 0 && results.size() > query.limit) {
        results.resize(query.limit);
      }
      report.trace.final_count = results.size();
      report.trace.total_ms = elapsed_ms(total_start);
      report.results = std::move(results);
      if (retrieval_cache_) {
        retrieval_cache_->put(cache_key, report.results);
      }
      publish_event(trace_id, "knowledge.retrieve.complete", {
        { "first_stage_count", std::to_string(report.trace.first_stage_count) },
        { "after_access_filter_count",
          std::to_string(report.trace.after_access_filter_count) },
        { "final_count", std::to_string(report.trace.final_count) },
        { "rewritten_query_count", std::to_string(report.trace.rewritten_query_count) },
        { "total_ms", std::to_string(report.trace.total_ms) },
      });
      return report;
    }
    catch (const std::exception& ex) {
      report.trace.total_ms = elapsed_ms(total_start);
      publish_event(trace_id, "knowledge.retrieve.failed", {
        { "query", query.text },
        { "error", ex.what() },
        { "total_ms", std::to_string(report.trace.total_ms) },
      });
      throw;
    }
  }

  bool erase_document(const std::string& document_id) {
    const bool erased_store = store_->erase_document(document_id);
    const bool erased_index = index_->erase_document(document_id);
    clear_retrieval_cache();
    return erased_store || erased_index;
  }

  void clear() {
    store_->clear();
    index_->clear();
    clear_retrieval_cache();
  }

  std::vector<knowledge_document> list_documents() const {
    return store_->list_documents();
  }

  std::size_t rebuild_index(knowledge_query query = {}) {
    query.limit = 0;
    auto chunks = store_->list_chunks(query);

    std::vector<std::string> texts;
    texts.reserve(chunks.size());
    for (const auto& chunk : chunks) {
      texts.push_back(chunk.content);
    }

    index_->clear();
    if (!chunks.empty()) {
      auto embeddings = embed_texts_in_batches(texts);
      if (embeddings.size() != chunks.size()) {
        throw std::runtime_error("knowledge embedding batch size mismatch");
      }
      for (std::size_t index = 0; index < chunks.size(); ++index) {
        apply_indexing_metadata(chunks[index], embeddings[index]);
      }
      index_->upsert_batch(chunks, embeddings);
    }
    clear_retrieval_cache();
    return chunks.size();
  }

  knowledge_rebuild_result rebuild_index_detailed(knowledge_query query = {}) {
    const auto trace_id = next_trace_id();
    publish_event(trace_id, "knowledge.rebuild.start", {});

    knowledge_rebuild_result result;
    try {
      query.limit = 0;
      const auto chunks = store_->list_chunks(query);

      result.scanned = chunks.size();

      index_->clear();
      for (const auto& chunk : chunks) {
        if (chunk.content.empty()) {
          ++result.skipped;
          continue;
        }

        try {
          auto indexed_chunk = chunk;
          auto embedding = embedding_model_->embed(chunk.content);
          apply_indexing_metadata(indexed_chunk, embedding);
          index_->upsert(indexed_chunk, embedding);
          ++result.rebuilt;
        }
        catch (const std::exception& ex) {
          result.errors.push_back(chunk.id + ": " + ex.what());
        }
      }
      publish_event(trace_id, "knowledge.rebuild.complete", {
        { "scanned", std::to_string(result.scanned) },
        { "rebuilt", std::to_string(result.rebuilt) },
        { "skipped", std::to_string(result.skipped) },
        { "errors", std::to_string(result.errors.size()) },
      });
      clear_retrieval_cache();
      return result;
    }
    catch (const std::exception& ex) {
      publish_event(trace_id, "knowledge.rebuild.failed", {
        { "scanned", std::to_string(result.scanned) },
        { "rebuilt", std::to_string(result.rebuilt) },
        { "skipped", std::to_string(result.skipped) },
        { "errors", std::to_string(result.errors.size()) },
        { "error", ex.what() },
      });
      throw;
    }
  }

  std::shared_ptr<knowledge_task<knowledge_rebuild_result>> rebuild_index_detailed_async(
    knowledge_query query = {},
    knowledge_task_progress_callback progress_callback = {},
    knowledge_task_policy task_policy = {}) {
    auto promise = std::make_shared<std::promise<knowledge_rebuild_result>>();
    auto task = std::make_shared<knowledge_task<knowledge_rebuild_result>>(promise->get_future());

    task->update_progress({
      .state = knowledge_task_state::pending,
      .message = "rebuild pending",
    });

    std::thread(
      [this, task, promise, query = std::move(query),
        progress_callback = std::move(progress_callback),
        task_policy]() mutable {
        knowledge_rebuild_result result;
        auto publish = [&](knowledge_task_progress progress) {
          progress.errors = result.errors;
          task->update_progress(progress);
          if (progress_callback) {
            progress_callback(progress);
          }
        };
        const auto trace_id = next_trace_id();

        try {
          publish_event(trace_id, "knowledge.rebuild.start", {
            { "async", "true" },
          });

          query.limit = 0;
          auto chunks = store_->list_chunks(query);
          result.scanned = chunks.size();

          publish({
            .state = knowledge_task_state::running,
            .completed = 0,
            .total = chunks.size(),
            .message = "rebuild running",
          });

          index_->clear();
          for (std::size_t index = 0; index < chunks.size(); ++index) {
            if (task->cancel_requested()) {
              publish_event(trace_id, "knowledge.rebuild.canceled", {
                { "scanned", std::to_string(result.scanned) },
                { "rebuilt", std::to_string(result.rebuilt) },
                { "skipped", std::to_string(result.skipped) },
                { "completed", std::to_string(index) },
              });
              publish({
                .state = knowledge_task_state::canceled,
                .completed = index,
                .total = chunks.size(),
                .message = "rebuild canceled",
              });
              clear_retrieval_cache();
              promise->set_value(std::move(result));
              return;
            }

            const auto& chunk = chunks[index];
            if (chunk.content.empty()) {
              ++result.skipped;
            }
            else {
              for (std::size_t attempt = 0; attempt <= task_policy.max_retries; ++attempt) {
                try {
                  auto indexed_chunk = chunk;
                  auto embedding = embedding_model_->embed(chunk.content);
                  apply_indexing_metadata(indexed_chunk, embedding);
                  index_->upsert(indexed_chunk, embedding);
                  ++result.rebuilt;
                  break;
                }
                catch (const std::exception& ex) {
                  if (attempt == task_policy.max_retries) {
                    result.errors.push_back(chunk.id + ": " + ex.what());
                  }
                  else {
                    std::this_thread::sleep_for(task_policy.retry_backoff);
                  }
                }
              }
            }

            publish({
              .state = knowledge_task_state::running,
              .completed = index + 1,
              .total = chunks.size(),
              .message = "rebuild running",
            });
          }

          publish({
            .state = knowledge_task_state::completed,
            .completed = chunks.size(),
            .total = chunks.size(),
            .message = "rebuild completed",
          });
          publish_event(trace_id, "knowledge.rebuild.complete", {
            { "async", "true" },
            { "scanned", std::to_string(result.scanned) },
            { "rebuilt", std::to_string(result.rebuilt) },
            { "skipped", std::to_string(result.skipped) },
            { "errors", std::to_string(result.errors.size()) },
          });
          clear_retrieval_cache();
          promise->set_value(std::move(result));
        }
        catch (const std::exception& ex) {
          result.errors.push_back(std::string("rebuild task failed: ") + ex.what());
          publish_event(trace_id, "knowledge.rebuild.failed", {
            { "async", "true" },
            { "scanned", std::to_string(result.scanned) },
            { "rebuilt", std::to_string(result.rebuilt) },
            { "skipped", std::to_string(result.skipped) },
            { "errors", std::to_string(result.errors.size()) },
            { "error", ex.what() },
          });
          publish({
            .state = knowledge_task_state::failed,
            .completed = result.rebuilt + result.skipped,
            .total = result.scanned,
            .message = ex.what(),
          });
          promise->set_value(std::move(result));
        }
      }).detach();

    return task;
  }

  std::vector<knowledge_result> expand_with_neighbors(
    std::vector<knowledge_result> results,
    std::size_t before,
    std::size_t after,
    knowledge_access_scope access = {}) const {
    if ((before == 0 && after == 0) || results.empty()) {
      return results;
    }

    knowledge_query all_chunks_query;
    all_chunks_query.limit = 0;
    all_chunks_query.access = std::move(access);
    auto chunks = store_->list_chunks(all_chunks_query);

    std::unordered_set<std::string> seen;
    std::vector<knowledge_result> expanded;
    expanded.reserve(results.size());

    for (const auto& result : results) {
      append_unique(expanded, seen, result);

      std::vector<knowledge_chunk> siblings;
      for (const auto& chunk : chunks) {
        if (chunk.document_id == result.chunk.document_id &&
            (!access_filter_ || access_filter_(chunk))) {
          siblings.push_back(chunk);
        }
      }

      std::sort(siblings.begin(), siblings.end(), [](const knowledge_chunk& lhs,
                                                      const knowledge_chunk& rhs) {
        return lhs.start_offset < rhs.start_offset;
      });

      auto it = std::find_if(siblings.begin(), siblings.end(), [&](const knowledge_chunk& chunk) {
        return chunk.id == result.chunk.id;
      });
      if (it == siblings.end()) {
        continue;
      }

      const auto index = static_cast<std::size_t>(std::distance(siblings.begin(), it));
      const auto first = index > before ? index - before : std::size_t {};
      const auto last = (std::min)(siblings.size() - 1, index + after);
      for (std::size_t sibling_index = first; sibling_index <= last; ++sibling_index) {
        knowledge_result neighbor {
          .chunk = siblings[sibling_index],
          .score = sibling_index == index ? result.score : result.score * 0.5,
          .vector_score = sibling_index == index ? result.vector_score : 0.0,
          .lexical_score = sibling_index == index ? result.lexical_score : 0.0,
        };
        append_unique(expanded, seen, std::move(neighbor));
      }
    }

    std::sort(expanded.begin(), expanded.end(), [](const knowledge_result& lhs,
                                                   const knowledge_result& rhs) {
      if (lhs.score != rhs.score) {
        return lhs.score > rhs.score;
      }
      if (lhs.chunk.document_id != rhs.chunk.document_id) {
        return lhs.chunk.document_id < rhs.chunk.document_id;
      }
      return lhs.chunk.start_offset < rhs.chunk.start_offset;
    });
    return expanded;
  }

  void set_reranker(std::shared_ptr<knowledge_reranker> reranker) {
    reranker_ = std::move(reranker);
    clear_retrieval_cache();
  }

  void set_access_filter(std::function<bool(const knowledge_chunk&)> filter) {
    access_filter_ = std::move(filter);
  }

  void set_event_sink(std::shared_ptr<knowledge_event_sink> sink) {
    std::scoped_lock lock(event_sink_mutex_);
    event_sink_ = std::move(sink);
  }

  void set_query_rewriter(std::shared_ptr<knowledge_query_rewriter> rewriter) {
    query_rewriter_ = std::move(rewriter);
    clear_retrieval_cache();
  }

  void set_retrieval_cache(std::shared_ptr<knowledge_retrieval_cache> cache) {
    retrieval_cache_ = std::move(cache);
  }

  const knowledge_indexing_policy& indexing_policy() const noexcept {
    return indexing_policy_;
  }

private:
  using clock = std::chrono::steady_clock;

  static double elapsed_ms(clock::time_point start) {
    return static_cast<double>(
      std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - start).count()) /
      1000.0;
  }

  static bool same_content_hash(
    const knowledge_document& lhs,
    const knowledge_document& rhs) {
    const auto lhs_hash = lhs.metadata.find("content_hash");
    const auto rhs_hash = rhs.metadata.find("content_hash");
    return lhs_hash != lhs.metadata.end() && rhs_hash != rhs.metadata.end() &&
           lhs_hash->second == rhs_hash->second;
  }

  static void append_unique(
    std::vector<knowledge_result>& results,
    std::unordered_set<std::string>& seen,
    knowledge_result result) {
    const auto key = result.chunk.id.empty()
                       ? result.chunk.document_id + ":" +
                           std::to_string(result.chunk.start_offset)
                       : result.chunk.id;
    if (seen.insert(key).second) {
      results.push_back(std::move(result));
    }
  }

  static void merge_results(
    std::vector<knowledge_result>& target,
    std::vector<knowledge_result> incoming) {
    for (auto& result : incoming) {
      const auto key = result.chunk.id.empty()
                         ? result.chunk.document_id + ":" +
                             std::to_string(result.chunk.start_offset)
                         : result.chunk.id;
      auto existing = std::find_if(target.begin(), target.end(), [&](const knowledge_result& item) {
        const auto existing_key = item.chunk.id.empty()
                                    ? item.chunk.document_id + ":" +
                                        std::to_string(item.chunk.start_offset)
                                    : item.chunk.id;
        return existing_key == key;
      });
      if (existing == target.end()) {
        target.push_back(std::move(result));
        continue;
      }
      if (result.score > existing->score) {
        *existing = std::move(result);
      }
    }
    std::sort(target.begin(), target.end(), [](const knowledge_result& lhs,
                                               const knowledge_result& rhs) {
      if (lhs.score != rhs.score) {
        return lhs.score > rhs.score;
      }
      if (lhs.chunk.document_id != rhs.chunk.document_id) {
        return lhs.chunk.document_id < rhs.chunk.document_id;
      }
      return lhs.chunk.start_offset < rhs.chunk.start_offset;
    });
  }

  static bool looks_like_broad_knowledge_query(std::string_view query) {
    const auto text = text_detail::lowercase_ascii(query);
    return text.find("main") != std::string::npos ||
           text.find("overview") != std::string::npos ||
           text.find("summary") != std::string::npos ||
           text.find("summarize") != std::string::npos ||
           text.find("list") != std::string::npos ||
           text.find("what are") != std::string::npos ||
           text.find("what is") != std::string::npos ||
           text.find("describe") != std::string::npos ||
           text.find("cover") != std::string::npos ||
           text.find("pattern") != std::string::npos;
  }

  static void apply_document_summary_boost(
    std::string_view query_text,
    std::vector<knowledge_result>& results) {
    if (!looks_like_broad_knowledge_query(query_text)) {
      return;
    }

    for (auto& result : results) {
      const auto chunking = result.chunk.metadata.find("chunking");
      if (chunking != result.chunk.metadata.end() &&
          chunking->second == "document_summary") {
        result.score += 1.0;
      }
    }
    std::sort(results.begin(), results.end(), [](const knowledge_result& lhs,
                                                 const knowledge_result& rhs) {
      if (lhs.score != rhs.score) {
        return lhs.score > rhs.score;
      }
      if (lhs.chunk.document_id != rhs.chunk.document_id) {
        return lhs.chunk.document_id < rhs.chunk.document_id;
      }
      return lhs.chunk.start_offset < rhs.chunk.start_offset;
    });
  }

  static std::string next_trace_id() {
    static std::atomic<std::uint64_t> next_id { 0 };
    return "knowledge-trace-" + std::to_string(++next_id);
  }

  void publish_event(
    const std::string& trace_id,
    std::string name,
    std::map<std::string, std::string> attributes) const {
    std::shared_ptr<knowledge_event_sink> sink;
    {
      std::scoped_lock lock(event_sink_mutex_);
      sink = event_sink_;
    }
    if (!sink) {
      return;
    }
    sink->publish({
      .trace_id = trace_id,
      .name = std::move(name),
      .attributes = std::move(attributes),
    });
  }

  void clear_retrieval_cache() const {
    if (retrieval_cache_) {
      retrieval_cache_->clear();
    }
  }

  void apply_indexing_metadata(
    knowledge_chunk& chunk,
    const std::vector<float>& embedding) const {
    if (indexing_policy_.expected_embedding_dimension != 0 &&
        embedding.size() != indexing_policy_.expected_embedding_dimension) {
      throw std::runtime_error(
        "knowledge embedding dimension mismatch: expected " +
        std::to_string(indexing_policy_.expected_embedding_dimension) +
        ", got " + std::to_string(embedding.size()));
    }
    if (!indexing_policy_.embedding_provider.empty()) {
      chunk.metadata["embedding_provider"] = indexing_policy_.embedding_provider;
    }
    if (!indexing_policy_.embedding_model.empty()) {
      chunk.metadata["embedding_model"] = indexing_policy_.embedding_model;
    }
    if (!indexing_policy_.embedding_version.empty()) {
      chunk.metadata["embedding_version"] = indexing_policy_.embedding_version;
    }
    chunk.metadata["embedding_dimension"] = std::to_string(embedding.size());
    chunk.metadata["index_schema_version"] =
      std::to_string(indexing_policy_.index_schema_version);
  }

  std::vector<std::vector<float>> embed_texts_in_batches(
    const std::vector<std::string>& texts) const {
    if (texts.empty()) {
      return {};
    }

    const auto batch_size =
      indexing_policy_.embedding_batch_size == 0
        ? texts.size()
        : indexing_policy_.embedding_batch_size;

    std::vector<std::vector<float>> embeddings;
    embeddings.reserve(texts.size());
    for (std::size_t offset = 0; offset < texts.size(); offset += batch_size) {
      const auto end = (std::min)(texts.size(), offset + batch_size);
      std::vector<std::string> batch;
      batch.reserve(end - offset);
      for (std::size_t index = offset; index < end; ++index) {
        batch.push_back(texts[index]);
      }

      auto batch_embeddings = embedding_model_->embed_batch(batch);
      if (batch_embeddings.size() != batch.size()) {
        throw std::runtime_error("knowledge embedding batch size mismatch");
      }
      for (auto& embedding : batch_embeddings) {
        embeddings.push_back(std::move(embedding));
      }
    }
    return embeddings;
  }

  std::shared_ptr<knowledge_store> store_;
  std::shared_ptr<knowledge_index> index_;
  std::shared_ptr<::wuwe::agent::memory::embedding_model> embedding_model_;
  std::shared_ptr<knowledge_reranker> reranker_;
  std::shared_ptr<knowledge_query_rewriter> query_rewriter_;
  std::shared_ptr<knowledge_retrieval_cache> retrieval_cache_;
  std::shared_ptr<knowledge_event_sink> event_sink_;
  mutable std::mutex event_sink_mutex_;
  std::function<bool(const knowledge_chunk&)> access_filter_;
  knowledge_splitter splitter_;
  knowledge_indexing_policy indexing_policy_;
  std::size_t next_document_id_ {};
};

} // namespace wuwe::agent::knowledge

#endif // WUWE_AGENT_KNOWLEDGE_RETRIEVER_HPP
