#ifndef WUWE_AGENT_MEMORY_CONTEXT_HPP
#define WUWE_AGENT_MEMORY_CONTEXT_HPP

#include <algorithm>
#include <cstddef>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <wuwe/agent/llm/llm_types.h>
#include <wuwe/agent/memory/embedding_model.hpp>
#include <wuwe/agent/memory/in_memory_store.hpp>
#include <wuwe/agent/memory/lexical_memory_ranker.hpp>
#include <wuwe/agent/memory/memory_policy.hpp>
#include <wuwe/agent/memory/vector_memory_index.hpp>

namespace wuwe::agent::memory {

namespace detail {

inline std::string normalize_memory_text(std::string text) {
  std::replace(text.begin(), text.end(), '\r', ' ');
  std::replace(text.begin(), text.end(), '\n', ' ');
  return text;
}

inline bool memory_record_visible_to_model(const memory_record& record) {
  if (record.visibility == memory_visibility::hidden) {
    return false;
  }

  const auto sensitivity = record.metadata.find("sensitivity");
  return sensitivity == record.metadata.end() || sensitivity->second != "secret";
}

inline bool same_record_key(const memory_record& lhs, const memory_record& rhs) {
  if (!lhs.id.empty() && !rhs.id.empty()) {
    return lhs.id == rhs.id && lhs.kind == rhs.kind &&
           lhs.scope.tenant_id == rhs.scope.tenant_id &&
           lhs.scope.user_id == rhs.scope.user_id &&
           lhs.scope.application_id == rhs.scope.application_id &&
           lhs.scope.conversation_id == rhs.scope.conversation_id &&
           lhs.scope.agent_id == rhs.scope.agent_id;
  }
  return lhs.kind == rhs.kind && lhs.content == rhs.content && lhs.summary == rhs.summary;
}

inline bool has_kind(const std::vector<memory_kind>& kinds, memory_kind kind) {
  return kinds.empty() || std::find(kinds.begin(), kinds.end(), kind) != kinds.end();
}

inline bool scope_has_anchor(const memory_scope& scope) {
  return !scope.tenant_id.empty() || !scope.user_id.empty() || !scope.conversation_id.empty();
}

inline bool scope_has_persistent_anchor(const memory_scope& scope) {
  return !scope.application_id.empty() && (!scope.tenant_id.empty() || !scope.user_id.empty());
}

inline bool content_matches_message(const memory_record& record, const chat_message& message) {
  if (record.kind != memory_kind::conversation) {
    return false;
  }
  if (record.content != message.content) {
    return false;
  }

  const auto role = record.metadata.find("message_role");
  if (role != record.metadata.end() && role->second != message.role) {
    return false;
  }

  const auto tool_call_id = record.metadata.find("tool_call_id");
  if (tool_call_id != record.metadata.end()) {
    return message.tool_call_id && tool_call_id->second == *message.tool_call_id;
  }

  return true;
}

inline std::size_t kind_limit(const memory_policy& policy, memory_kind kind) {
  switch (kind) {
  case memory_kind::conversation:
    return policy.max_recent_messages;
  case memory_kind::working:
    return policy.max_working_records;
  case memory_kind::summary:
    return policy.max_summary_records;
  case memory_kind::long_term:
    return policy.max_long_term_records;
  case memory_kind::retrieved:
    return policy.max_long_term_records;
  }

  return 0;
}

inline bool kind_enabled(const memory_policy& policy, memory_kind kind) {
  switch (kind) {
  case memory_kind::conversation:
    return policy.include_conversation;
  case memory_kind::working:
    return policy.include_working;
  case memory_kind::summary:
    return policy.include_summaries;
  case memory_kind::long_term:
  case memory_kind::retrieved:
    return policy.include_long_term;
  }

  return false;
}

} // namespace detail

struct memory_rebuild_error {
  std::string memory_id;
  std::string message;
};

struct memory_rebuild_result {
  std::size_t scanned { 0 };
  std::size_t rebuilt { 0 };
  std::size_t skipped_empty { 0 };
  std::size_t skipped_hidden_or_secret { 0 };
  std::vector<memory_rebuild_error> errors;
};

struct conversation_summary_options {
  memory_scope scope;
  std::size_t max_source_records { 64 };
  bool erase_source_records { false };
  std::map<std::string, std::string> metadata;
};

struct conversation_summary_result {
  std::optional<memory_record> summary;
  std::size_t source_count { 0 };
};

struct memory_compaction_error {
  std::string memory_id;
  std::string message;
};

struct memory_compaction_result {
  std::size_t scanned { 0 };
  std::size_t erased_expired { 0 };
  std::vector<memory_compaction_error> errors;
};

enum class memory_audit_action {
  remember,
  update,
  erase,
  clear,
  compact_expired,
  rebuild_index,
  reconcile_index,
  summarize_conversation,
  reject,
};

inline std::string to_string(memory_audit_action action) {
  switch (action) {
  case memory_audit_action::remember:
    return "remember";
  case memory_audit_action::update:
    return "update";
  case memory_audit_action::erase:
    return "erase";
  case memory_audit_action::clear:
    return "clear";
  case memory_audit_action::compact_expired:
    return "compact_expired";
  case memory_audit_action::rebuild_index:
    return "rebuild_index";
  case memory_audit_action::reconcile_index:
    return "reconcile_index";
  case memory_audit_action::summarize_conversation:
    return "summarize_conversation";
  case memory_audit_action::reject:
    return "reject";
  }
  return "unknown";
}

struct memory_audit_event {
  memory_audit_action action { memory_audit_action::remember };
  bool success { true };
  std::string memory_id;
  memory_kind kind { memory_kind::working };
  memory_scope scope;
  std::string message;
  std::map<std::string, std::string> metadata;
};

class memory_context {
public:
  memory_context()
      : short_term_store_(std::make_shared<in_memory_store>()),
        long_term_store_(std::make_shared<in_memory_store>()),
        ranker_(std::make_shared<lexical_memory_ranker>()) {
  }

  explicit memory_context(memory_policy policy)
      : short_term_store_(std::make_shared<in_memory_store>()),
        long_term_store_(std::make_shared<in_memory_store>()),
        policy_(std::move(policy)),
        ranker_(std::make_shared<lexical_memory_ranker>()) {
  }

  memory_context(
    std::shared_ptr<memory_store> short_term_store,
    std::shared_ptr<memory_store> long_term_store,
    memory_policy policy = {},
    std::shared_ptr<memory_ranker> ranker = {})
      : short_term_store_(std::move(short_term_store)),
        long_term_store_(std::move(long_term_store)),
        policy_(std::move(policy)),
        ranker_(std::move(ranker)) {
    if (!short_term_store_) {
      short_term_store_ = std::make_shared<in_memory_store>();
    }
    if (!long_term_store_) {
      long_term_store_ = short_term_store_;
    }
    if (!ranker_) {
      ranker_ = std::make_shared<lexical_memory_ranker>();
    }
  }

  memory_record remember(memory_record record) {
    if (record.scope.application_id.empty() && !active_scope_.application_id.empty()) {
      record.scope = active_scope_;
    }
    apply_retention_policy(record);
    enforce_privacy_policy(record, memory_audit_action::remember);

    if (record.kind == memory_kind::long_term || record.kind == memory_kind::retrieved) {
      if (policy_.require_scope_for_long_term && !detail::scope_has_persistent_anchor(record.scope)) {
        throw std::invalid_argument(
          "long-term memory requires application_id and tenant_id or user_id");
      }
      auto saved = long_term_store_->add(std::move(record));
      mark_index_status(saved, index_record(saved));
      emit_audit(memory_audit_action::remember, true, saved);
      return saved;
    }

    auto saved = short_term_store_->add(std::move(record));
    emit_audit(memory_audit_action::remember, true, saved);
    return saved;
  }

  bool update(memory_record record) {
    if (record.scope.application_id.empty() && !active_scope_.application_id.empty()) {
      record.scope = active_scope_;
    }

    const bool durable =
      record.kind == memory_kind::long_term || record.kind == memory_kind::retrieved;
    if (durable && policy_.require_scope_for_long_term &&
        !detail::scope_has_persistent_anchor(record.scope)) {
      throw std::invalid_argument(
        "long-term memory requires application_id and tenant_id or user_id");
    }
    apply_retention_policy(record);
    enforce_privacy_policy(record, memory_audit_action::update);

    auto& store = durable ? *long_term_store_ : *short_term_store_;
    if (!store.update(record)) {
      emit_audit(memory_audit_action::update, false, record, "record not found");
      return false;
    }

    if (durable) {
      if (record.content.empty() || !detail::memory_record_visible_to_model(record)) {
        erase_index(record.id, record.scope);
      }
      else {
        mark_index_status(record, index_record(record));
      }
    }
    else {
      erase_index(record.id, record.scope);
    }
    emit_audit(memory_audit_action::update, true, record);
    return true;
  }

  bool erase(
    const std::string& id,
    const memory_scope& scope,
    memory_kind kind = memory_kind::long_term) {
    const bool durable = kind == memory_kind::long_term || kind == memory_kind::retrieved;
    auto& store = durable ? *long_term_store_ : *short_term_store_;
    const bool erased_store = store.erase(id, scope);
    const bool erased_index = durable ? erase_index(id, scope) : false;
    memory_record audit_record;
    audit_record.id = id;
    audit_record.kind = kind;
    audit_record.scope = scope;
    emit_audit(memory_audit_action::erase, erased_store || erased_index, audit_record);
    return erased_store || erased_index;
  }

  std::size_t clear(const memory_scope& scope) {
    const auto erased_short = short_term_store_->clear(scope);
    const auto erased_long =
      long_term_store_ == short_term_store_ ? 0 : long_term_store_->clear(scope);
    clear_index(scope);
    memory_record audit_record;
    audit_record.scope = scope;
    emit_audit(memory_audit_action::clear, true, audit_record,
      "erased=" + std::to_string(erased_short + erased_long));
    return erased_short + erased_long;
  }

  memory_compaction_result compact_expired(memory_query query = {}) {
    memory_compaction_result result;

    if (!detail::scope_has_anchor(query.scope)) {
      query.scope = active_scope_;
    }
    if (policy_.require_scoped_recall && !detail::scope_has_anchor(query.scope)) {
      return result;
    }
    if (query.limit == 0 || query.limit == memory_query {}.limit) {
      query.limit = (std::numeric_limits<std::size_t>::max)();
    }
    query.include_expired = true;

    const auto records = list(query);
    const auto now = std::chrono::system_clock::now();
    for (const auto& record : records) {
      ++result.scanned;
      if (!record.expires_at || *record.expires_at > now) {
        continue;
      }

      try {
        if (erase(record.id, record.scope, record.kind)) {
          ++result.erased_expired;
        }
      }
      catch (const std::exception& ex) {
        result.errors.push_back({
          .memory_id = record.id,
          .message = ex.what(),
        });
      }
      catch (...) {
        result.errors.push_back({
          .memory_id = record.id,
          .message = "unknown compaction error",
        });
      }
    }
    memory_record audit_record;
    audit_record.scope = query.scope;
    emit_audit(memory_audit_action::compact_expired, result.errors.empty(), audit_record,
      "scanned=" + std::to_string(result.scanned) +
        " erased_expired=" + std::to_string(result.erased_expired));
    return result;
  }

  memory_record remember_working(
    std::string content,
    std::map<std::string, std::string> metadata = {}) {
    memory_record record;
    record.kind = memory_kind::working;
    record.content = std::move(content);
    record.scope = active_scope_;
    record.metadata = std::move(metadata);
    return remember(std::move(record));
  }

  memory_record remember_summary(
    std::string content,
    std::map<std::string, std::string> metadata = {}) {
    memory_record record;
    record.kind = memory_kind::summary;
    record.content = std::move(content);
    record.scope = active_scope_;
    record.metadata = std::move(metadata);
    return remember(std::move(record));
  }

  memory_record remember_long_term(
    std::string content,
    memory_scope scope,
    std::map<std::string, std::string> metadata = {}) {
    memory_record record;
    record.kind = memory_kind::long_term;
    record.content = std::move(content);
    record.scope = std::move(scope);
    record.metadata = std::move(metadata);
    return remember(std::move(record));
  }

  std::optional<memory_record> get(
    const std::string& id,
    const memory_scope& scope,
    memory_kind kind = memory_kind::long_term) const {
    const auto& store =
      (kind == memory_kind::long_term || kind == memory_kind::retrieved)
      ? *long_term_store_
      : *short_term_store_;
    return store.get(id, scope);
  }

  std::vector<memory_record> list(memory_query query = {}) const {
    if (!detail::scope_has_anchor(query.scope)) {
      query.scope = active_scope_;
    }
    if (policy_.require_scoped_recall && !detail::scope_has_anchor(query.scope)) {
      return {};
    }
    const auto final_limit = query.limit == 0 ? policy_.max_long_term_records : query.limit;

    std::vector<memory_record> result;
    auto append_unique = [&](std::vector<memory_record> records) {
      for (auto& record : records) {
        const bool exists = std::any_of(result.begin(), result.end(), [&](const auto& item) {
          return detail::same_record_key(item, record);
        });
        if (!exists) {
          result.push_back(std::move(record));
        }
      }
    };

    const bool include_short =
      query.kinds.empty() ||
      detail::has_kind(query.kinds, memory_kind::conversation) ||
      detail::has_kind(query.kinds, memory_kind::working) ||
      detail::has_kind(query.kinds, memory_kind::summary);
    const bool include_long =
      query.kinds.empty() ||
      detail::has_kind(query.kinds, memory_kind::long_term) ||
      detail::has_kind(query.kinds, memory_kind::retrieved);

    memory_query store_query = query;
    store_query.limit = (std::max)(final_limit, final_limit * 2);

    if (include_short) {
      append_unique(short_term_store_->search(store_query));
    }
    if (include_long && long_term_store_ != short_term_store_) {
      append_unique(long_term_store_->search(store_query));
    }

    query.limit = final_limit;
    if (ranker_) {
      result = ranker_->rank(query, std::move(result));
    }
    return result;
  }

  void observe(const chat_message& message, const memory_scope& scope = {}) {
    if (message.content.empty() && message.tool_calls.empty()) {
      return;
    }

    memory_record record;
    record.kind = memory_kind::conversation;
    record.content = message.content;
    record.scope = scope.application_id.empty() ? active_scope_ : scope;
    record.metadata["message_role"] = message.role;
    if (message.name) {
      record.metadata["name"] = *message.name;
    }
    if (message.tool_call_id) {
      record.metadata["tool_call_id"] = *message.tool_call_id;
    }
    if (!message.tool_calls.empty()) {
      record.metadata["tool_call_count"] = std::to_string(message.tool_calls.size());
    }

    short_term_store_->add(std::move(record));
  }

  conversation_summary_result summarize_conversation(
    const conversation_summary_options& options,
    const std::function<std::string(const std::vector<memory_record>&)>& summarizer) {
    conversation_summary_result result;
    if (!summarizer) {
      return result;
    }

    memory_query query;
    query.scope = detail::scope_has_anchor(options.scope) ? options.scope : active_scope_;
    query.kinds = { memory_kind::conversation };
    query.limit = options.max_source_records == 0 ? 64 : options.max_source_records;

    auto records = short_term_store_->search(query);
    std::sort(records.begin(), records.end(), [](const auto& lhs, const auto& rhs) {
      return lhs.created_at < rhs.created_at;
    });

    result.source_count = records.size();
    if (records.empty()) {
      return result;
    }

    auto summary_text = summarizer(records);
    if (summary_text.empty()) {
      return result;
    }

    memory_record summary;
    summary.kind = memory_kind::summary;
    summary.content = std::move(summary_text);
    summary.scope = query.scope;
    summary.metadata = options.metadata;
    summary.metadata["source"] = "conversation_summary";
    summary.metadata["source_count"] = std::to_string(records.size());
    result.summary = remember(std::move(summary));
    if (result.summary) {
      emit_audit(memory_audit_action::summarize_conversation, true, *result.summary,
        "source_count=" + std::to_string(records.size()));
    }

    if (options.erase_source_records) {
      for (const auto& record : records) {
        short_term_store_->erase(record.id, record.scope);
      }
    }

    return result;
  }

  std::vector<memory_record> recall(const memory_query& query) const {
    if (policy_.require_scoped_recall && !detail::scope_has_anchor(query.scope)) {
      return {};
    }

    std::vector<memory_record> result;

    auto append_unique = [&](std::vector<memory_record> records) {
      for (auto& record : records) {
        const bool exists = std::any_of(result.begin(), result.end(), [&](const memory_record& item) {
          return detail::same_record_key(item, record);
        });
        if (!exists) {
          result.push_back(std::move(record));
        }
      }
    };

    memory_query store_query = query;
    store_query.limit = (std::max)(query.limit, query.limit * 2);

    append_unique(vector_recall(store_query));
    append_unique(short_term_store_->search(store_query));
    if (long_term_store_ != short_term_store_) {
      append_unique(long_term_store_->search(store_query));
    }

    if (ranker_) {
      result = ranker_->rank(query, std::move(result));
    }

    return result;
  }

  std::size_t rebuild_vector_index(memory_query query = {}) const {
    return rebuild_vector_index_detailed(std::move(query)).rebuilt;
  }

  memory_rebuild_result reconcile_pending_reindex(memory_query query = {}) const {
    if (!detail::scope_has_anchor(query.scope)) {
      query.scope = active_scope_;
    }
    query.kinds = { memory_kind::long_term, memory_kind::retrieved };
    query.filters["index_status"] = "pending_reindex";
    auto result = rebuild_vector_index_detailed(std::move(query));
    memory_record audit_record;
    audit_record.scope = query.scope;
    emit_audit(memory_audit_action::reconcile_index, result.errors.empty(), audit_record,
      "scanned=" + std::to_string(result.scanned) +
        " rebuilt=" + std::to_string(result.rebuilt));
    return result;
  }

  memory_rebuild_result rebuild_vector_index_detailed(memory_query query = {}) const {
    memory_rebuild_result result;
    if (!embedding_model_ || !vector_index_) {
      return result;
    }

    const bool default_query =
      query.text.empty() && !detail::scope_has_anchor(query.scope) && query.kinds.empty() &&
      query.filters.empty() && !query.include_expired && query.limit == memory_query {}.limit;

    if (!detail::scope_has_anchor(query.scope)) {
      query.scope = active_scope_;
    }
    if (policy_.require_scoped_recall && !detail::scope_has_anchor(query.scope)) {
      return result;
    }
    if (query.kinds.empty()) {
      query.kinds = { memory_kind::long_term, memory_kind::retrieved };
    }
    if (query.limit == 0 || default_query) {
      query.limit = (std::numeric_limits<std::size_t>::max)();
    }

    vector_index_->clear(query.scope);

    std::vector<memory_record> batch_records;
    std::vector<std::string> batch_texts;
    const auto batch_size = policy_.vector_rebuild_batch_size == 0
      ? std::size_t { 1 }
      : policy_.vector_rebuild_batch_size;

    const auto flush_batch = [&] {
      if (batch_records.empty()) {
        return;
      }
      try {
        const auto embeddings = embedding_model_->embed_batch(batch_texts);
        vector_index_->upsert_batch(batch_records, embeddings);
        result.rebuilt += batch_records.size();
        for (auto& record : batch_records) {
          record.metadata["index_status"] = "indexed";
          record.metadata.erase("index_error");
          long_term_store_->update(record);
        }
      }
      catch (const std::exception& ex) {
        for (auto& record : batch_records) {
          record.metadata["index_status"] = "pending_reindex";
          record.metadata["index_error"] = ex.what();
          long_term_store_->update(record);
          result.errors.push_back({
            .memory_id = record.id,
            .message = ex.what(),
          });
        }
      }
      catch (...) {
        for (auto& record : batch_records) {
          record.metadata["index_status"] = "pending_reindex";
          record.metadata["index_error"] = "unknown rebuild error";
          long_term_store_->update(record);
          result.errors.push_back({
            .memory_id = record.id,
            .message = "unknown rebuild error",
          });
        }
      }
      batch_records.clear();
      batch_texts.clear();
    };

    for (const auto& record : long_term_store_->search(query)) {
      ++result.scanned;
      if (record.content.empty()) {
        ++result.skipped_empty;
        continue;
      }
      if (!detail::memory_record_visible_to_model(record)) {
        ++result.skipped_hidden_or_secret;
        continue;
      }

      batch_records.push_back(record);
      batch_texts.push_back(record.content);
      if (batch_records.size() >= batch_size) {
        flush_batch();
      }
    }
    flush_batch();
    memory_record audit_record;
    audit_record.scope = query.scope;
    emit_audit(memory_audit_action::rebuild_index, result.errors.empty(), audit_record,
      "scanned=" + std::to_string(result.scanned) +
        " rebuilt=" + std::to_string(result.rebuilt));
    return result;
  }

  llm_request augment(llm_request request, std::string_view query_text) const {
    const std::string memory_block = build_memory_block(std::string(query_text), &request.messages);
    if (memory_block.empty()) {
      return request;
    }

    chat_message memory_message {
      .role = policy_.inject_as_system_message ? "system" : "user",
      .content = memory_block,
    };

    if (!policy_.inject_as_system_message) {
      request.messages.insert(request.messages.begin(), std::move(memory_message));
      return request;
    }

    auto insert_at = request.messages.begin();
    while (insert_at != request.messages.end() && insert_at->role == "system") {
      ++insert_at;
    }
    request.messages.insert(insert_at, std::move(memory_message));
    return request;
  }

  std::string build_memory_block(
    const std::string& query_text,
    const std::vector<chat_message>* request_messages = nullptr) const {
    std::vector<memory_record> selected;
    std::size_t remaining = policy_.max_memory_chars;
    if (policy_.max_memory_tokens != 0 && policy_.estimated_chars_per_token != 0) {
      remaining = (std::min)(remaining, policy_.max_memory_tokens * policy_.estimated_chars_per_token);
    }

    const auto add_kind = [&](memory_kind kind) {
      if (!detail::kind_enabled(policy_, kind) || remaining == 0) {
        return;
      }

      memory_query query;
      query.text = query_text;
      query.scope = active_scope_;
      query.kinds = { kind };
      query.limit = detail::kind_limit(policy_, kind);

      for (auto& record : recall(query)) {
        if (!detail::memory_record_visible_to_model(record)) {
          continue;
        }
        if (policy_.dedupe_request_messages && request_messages) {
          const bool already_in_request =
            std::any_of(request_messages->begin(), request_messages->end(), [&](const chat_message& message) {
              return detail::content_matches_message(record, message);
            });
          if (already_in_request) {
            continue;
          }
        }

        std::string text = record.summary.empty() ? record.content : record.summary;
        text = detail::normalize_memory_text(std::move(text));
        if (text.empty()) {
          continue;
        }

        if (text.size() > policy_.max_record_chars) {
          if (!record.summary.empty()) {
            continue;
          }
          text = text.substr(0, policy_.max_record_chars);
          text += "...";
        }

        const std::size_t line_size = to_string(record.kind).size() + text.size() + 6;
        if (line_size > remaining) {
          continue;
        }

        record.content = std::move(text);
        record.summary.clear();
        selected.push_back(std::move(record));
        remaining -= line_size;
      }
    };

    add_kind(memory_kind::summary);
    add_kind(memory_kind::long_term);
    add_kind(memory_kind::working);
    add_kind(memory_kind::conversation);

    if (selected.empty()) {
      return {};
    }

    std::ostringstream output;
    output << policy_.injection_header;
    for (const auto& record : selected) {
      output << "\n- [" << to_string(record.kind) << "] " << record.content;
    }
    return output.str();
  }

  const memory_policy& policy() const noexcept {
    return policy_;
  }

  void set_policy(memory_policy policy) {
    policy_ = std::move(policy);
  }

  void set_ranker(std::shared_ptr<memory_ranker> ranker) {
    ranker_ = std::move(ranker);
    if (!ranker_) {
      ranker_ = std::make_shared<lexical_memory_ranker>();
    }
  }

  void set_embedding_model(std::shared_ptr<embedding_model> model) {
    embedding_model_ = std::move(model);
  }

  void set_vector_index(std::shared_ptr<vector_memory_index> index) {
    vector_index_ = std::move(index);
  }

  void set_audit_sink(std::function<void(const memory_audit_event&)> sink) {
    audit_sink_ = std::move(sink);
  }

  void set_privacy_filter(std::function<bool(memory_record&, std::string&)> filter) {
    privacy_filter_ = std::move(filter);
  }

  const memory_scope& scope() const noexcept {
    return active_scope_;
  }

  void set_scope(memory_scope scope) {
    active_scope_ = std::move(scope);
  }

private:
  void apply_retention_policy(memory_record& record) const {
    if (record.expires_at) {
      return;
    }

    const auto ttl = (record.kind == memory_kind::long_term || record.kind == memory_kind::retrieved)
      ? policy_.default_long_term_ttl
      : policy_.default_working_ttl;
    if (ttl.count() > 0) {
      record.expires_at = std::chrono::system_clock::now() + ttl;
    }
  }

  void enforce_privacy_policy(memory_record& record, memory_audit_action action) {
    if (!privacy_filter_) {
      return;
    }

    std::string reason;
    if (privacy_filter_(record, reason)) {
      return;
    }

    emit_audit(memory_audit_action::reject, false, record,
      reason.empty() ? "memory rejected by privacy filter" : reason);
    throw std::invalid_argument(
      reason.empty() ? "memory rejected by privacy filter" : reason);
  }

  void emit_audit(
    memory_audit_action action,
    bool success,
    const memory_record& record,
    std::string message = {}) const {
    if (!audit_sink_) {
      return;
    }

    audit_sink_({
      .action = action,
      .success = success,
      .memory_id = record.id,
      .kind = record.kind,
      .scope = record.scope,
      .message = std::move(message),
      .metadata = record.metadata,
    });
  }

  std::optional<std::string> index_record(const memory_record& record) {
    if (!embedding_model_ || !vector_index_ || record.content.empty() ||
        !detail::memory_record_visible_to_model(record)) {
      return std::nullopt;
    }

    try {
      vector_index_->upsert(record, embedding_model_->embed(record.content));
      return std::nullopt;
    }
    catch (const std::exception& ex) {
      if (policy_.throw_on_vector_index_error) {
        throw;
      }
      return ex.what();
    }
    catch (...) {
      if (policy_.throw_on_vector_index_error) {
        throw;
      }
      return "unknown vector index error";
    }
  }

  void mark_index_status(memory_record& record, std::optional<std::string> error) {
    if (!embedding_model_ || !vector_index_ || record.content.empty() ||
        !detail::memory_record_visible_to_model(record)) {
      return;
    }

    if (error) {
      record.metadata["index_status"] = "pending_reindex";
      record.metadata["index_error"] = *error;
    }
    else {
      record.metadata["index_status"] = "indexed";
      record.metadata.erase("index_error");
    }
    long_term_store_->update(record);
  }

  bool erase_index(const std::string& id, const memory_scope& scope) {
    if (!vector_index_) {
      return false;
    }
    return vector_index_->erase(id, scope);
  }

  std::size_t clear_index(const memory_scope& scope) {
    if (!vector_index_) {
      return 0;
    }
    return vector_index_->clear(scope);
  }

  std::vector<memory_record> vector_recall(const memory_query& query) const {
    if (!embedding_model_ || !vector_index_ || query.text.empty()) {
      return {};
    }

    const bool can_search_long_term =
      detail::has_kind(query.kinds, memory_kind::long_term) ||
      detail::has_kind(query.kinds, memory_kind::retrieved);
    if (!can_search_long_term) {
      return {};
    }

    vector_memory_query vector_query;
    vector_query.embedding = embedding_model_->embed(query.text);
    vector_query.scope = query.scope;
    vector_query.kinds = query.kinds.empty()
      ? std::vector<memory_kind> { memory_kind::long_term, memory_kind::retrieved }
      : query.kinds;
    vector_query.limit = query.limit;
    vector_query.filters = query.filters;
    vector_query.include_expired = query.include_expired;

    std::vector<memory_record> result;
    for (const auto& match : vector_index_->search(vector_query)) {
      auto record = long_term_store_->get(match.memory_id, query.scope);
      if (!record) {
        continue;
      }
      if (!detail::has_kind(vector_query.kinds, record->kind)) {
        continue;
      }
      if (!query.include_expired && detail::is_expired(*record)) {
        continue;
      }
      if (!detail::metadata_matches(record->metadata, query.filters)) {
        continue;
      }

      record->score = match.score;
      record->metadata["vector_score"] = std::to_string(match.score);
      result.push_back(std::move(*record));
    }
    return result;
  }

private:
  std::shared_ptr<memory_store> short_term_store_;
  std::shared_ptr<memory_store> long_term_store_;
  std::shared_ptr<memory_ranker> ranker_;
  std::shared_ptr<embedding_model> embedding_model_;
  std::shared_ptr<vector_memory_index> vector_index_;
  std::function<void(const memory_audit_event&)> audit_sink_;
  std::function<bool(memory_record&, std::string&)> privacy_filter_;
  memory_policy policy_;
  memory_scope active_scope_;
};

} // namespace wuwe::agent::memory

#endif // WUWE_AGENT_MEMORY_CONTEXT_HPP
