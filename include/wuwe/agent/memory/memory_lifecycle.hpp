#ifndef WUWE_AGENT_MEMORY_LIFECYCLE_HPP
#define WUWE_AGENT_MEMORY_LIFECYCLE_HPP

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <wuwe/agent/memory/memory_record.hpp>

namespace wuwe::agent::memory {

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

} // namespace wuwe::agent::memory

#endif // WUWE_AGENT_MEMORY_LIFECYCLE_HPP
