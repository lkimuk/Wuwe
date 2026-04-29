#ifndef WUWE_AGENT_MEMORY_CONTEXT_HPP
#define WUWE_AGENT_MEMORY_CONTEXT_HPP

#include <algorithm>
#include <cstddef>
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
#include <wuwe/agent/memory/in_memory_store.hpp>
#include <wuwe/agent/memory/lexical_memory_ranker.hpp>
#include <wuwe/agent/memory/memory_policy.hpp>

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
    return lhs.id == rhs.id;
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

    if (record.kind == memory_kind::long_term || record.kind == memory_kind::retrieved) {
      if (policy_.require_scope_for_long_term && !detail::scope_has_persistent_anchor(record.scope)) {
        throw std::invalid_argument(
          "long-term memory requires application_id and tenant_id or user_id");
      }
      return long_term_store_->add(std::move(record));
    }

    return short_term_store_->add(std::move(record));
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

    append_unique(short_term_store_->search(store_query));
    if (long_term_store_ != short_term_store_) {
      append_unique(long_term_store_->search(store_query));
    }

    if (ranker_) {
      result = ranker_->rank(query, std::move(result));
    }

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

  const memory_scope& scope() const noexcept {
    return active_scope_;
  }

  void set_scope(memory_scope scope) {
    active_scope_ = std::move(scope);
  }

private:
  std::shared_ptr<memory_store> short_term_store_;
  std::shared_ptr<memory_store> long_term_store_;
  std::shared_ptr<memory_ranker> ranker_;
  memory_policy policy_;
  memory_scope active_scope_;
};

} // namespace wuwe::agent::memory

#endif // WUWE_AGENT_MEMORY_CONTEXT_HPP
