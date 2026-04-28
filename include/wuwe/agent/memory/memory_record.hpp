#ifndef WUWE_AGENT_MEMORY_RECORD_HPP
#define WUWE_AGENT_MEMORY_RECORD_HPP

#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace wuwe::agent::memory {

enum class memory_kind {
  conversation,
  working,
  summary,
  long_term,
  retrieved,
};

enum class memory_visibility {
  visible,
  hidden,
};

struct memory_scope {
  std::string tenant_id;
  std::string user_id;
  std::string application_id;
  std::string conversation_id;
  std::string agent_id;
};

struct memory_record {
  std::string id;
  memory_kind kind { memory_kind::working };
  memory_visibility visibility { memory_visibility::visible };

  std::string content;
  std::string summary;

  memory_scope scope;

  double score { 0.0 };
  int priority { 0 };

  std::chrono::system_clock::time_point created_at {};
  std::chrono::system_clock::time_point updated_at {};
  std::optional<std::chrono::system_clock::time_point> expires_at;

  std::map<std::string, std::string> metadata;
};

struct memory_query {
  std::string text;
  memory_scope scope;

  std::vector<memory_kind> kinds;
  std::size_t limit { 8 };

  std::map<std::string, std::string> filters;
  bool include_expired { false };
};

inline std::string to_string(memory_kind kind) {
  switch (kind) {
  case memory_kind::conversation:
    return "conversation";
  case memory_kind::working:
    return "working";
  case memory_kind::summary:
    return "summary";
  case memory_kind::long_term:
    return "long_term";
  case memory_kind::retrieved:
    return "retrieved";
  }

  return "unknown";
}

inline std::string to_string(memory_visibility visibility) {
  switch (visibility) {
  case memory_visibility::visible:
    return "visible";
  case memory_visibility::hidden:
    return "hidden";
  }

  return "unknown";
}

inline bool scope_matches(const memory_scope& record_scope, const memory_scope& query_scope) {
  const auto matches_field = [](const std::string& record_value, const std::string& query_value) {
    return query_value.empty() || record_value == query_value;
  };

  return matches_field(record_scope.tenant_id, query_scope.tenant_id) &&
         matches_field(record_scope.user_id, query_scope.user_id) &&
         matches_field(record_scope.application_id, query_scope.application_id) &&
         matches_field(record_scope.conversation_id, query_scope.conversation_id) &&
         matches_field(record_scope.agent_id, query_scope.agent_id);
}

} // namespace wuwe::agent::memory

#endif // WUWE_AGENT_MEMORY_RECORD_HPP
