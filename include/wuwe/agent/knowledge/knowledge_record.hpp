#ifndef WUWE_AGENT_KNOWLEDGE_RECORD_HPP
#define WUWE_AGENT_KNOWLEDGE_RECORD_HPP

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace wuwe::agent::knowledge {

struct knowledge_document {
  std::string id;
  std::string title;
  std::string content;
  std::string source_uri;
  std::map<std::string, std::string> metadata;
};

struct knowledge_chunk {
  std::string id;
  std::string document_id;
  std::string title;
  std::string content;
  std::size_t start_offset {};
  std::size_t end_offset {};
  std::size_t start_line {};
  std::size_t end_line {};
  std::string source_uri;
  std::map<std::string, std::string> metadata;
};

struct knowledge_access_scope {
  std::string tenant_id;
  std::string user_id;
  std::vector<std::string> roles;
  bool bypass_acl {};
};

struct knowledge_query {
  std::string text;
  std::size_t limit { 6 };
  std::size_t candidate_limit {};
  std::map<std::string, std::string> filters;
  knowledge_access_scope access;
  double minimum_score { 0.0 };
  double vector_weight { 1.0 };
  double lexical_weight { 0.25 };
};

struct knowledge_result {
  knowledge_chunk chunk;
  double score {};
  double vector_score {};
  double lexical_score {};
};

inline bool metadata_matches(
  const std::map<std::string, std::string>& metadata,
  const std::map<std::string, std::string>& filters) {
  for (const auto& [key, value] : filters) {
    const auto it = metadata.find(key);
    if (it == metadata.end() || it->second != value) {
      return false;
    }
  }
  return true;
}

inline bool csv_contains(std::string_view csv, const std::string& value) {
  std::size_t start = 0;
  while (start <= csv.size()) {
    const auto end = csv.find(',', start);
    auto token = csv.substr(start, end == std::string_view::npos ? csv.size() - start : end - start);
    while (!token.empty() && token.front() == ' ') {
      token.remove_prefix(1);
    }
    while (!token.empty() && token.back() == ' ') {
      token.remove_suffix(1);
    }
    if (token == value) {
      return true;
    }
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }
  return false;
}

inline bool metadata_access_matches(
  const std::map<std::string, std::string>& metadata,
  const knowledge_access_scope& access) {
  if (access.bypass_acl) {
    return true;
  }

  if (const auto tenant = metadata.find("tenant_id");
      tenant != metadata.end() && tenant->second != access.tenant_id) {
    return false;
  }
  if (const auto user = metadata.find("user_id");
      user != metadata.end() && !user->second.empty() && user->second != access.user_id) {
    return false;
  }
  if (const auto users = metadata.find("allowed_users");
      users != metadata.end() && !users->second.empty() &&
      !csv_contains(users->second, access.user_id)) {
    return false;
  }
  if (const auto roles = metadata.find("allowed_roles");
      roles != metadata.end() && !roles->second.empty()) {
    for (const auto& role : access.roles) {
      if (csv_contains(roles->second, role)) {
        return true;
      }
    }
    return false;
  }
  return true;
}

} // namespace wuwe::agent::knowledge

#endif // WUWE_AGENT_KNOWLEDGE_RECORD_HPP
