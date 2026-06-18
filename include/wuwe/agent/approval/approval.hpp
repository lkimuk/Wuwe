#ifndef WUWE_AGENT_APPROVAL_APPROVAL_HPP
#define WUWE_AGENT_APPROVAL_APPROVAL_HPP

#include <map>
#include <string>
#include <vector>

#include <wuwe/agent/capability/capability.hpp>

namespace wuwe::agent::approval {

enum class approval_decision_kind {
  approved,
  denied,
  needs_manual_review,
};

enum class approval_scope {
  once,
  session,
  workspace,
};

struct approval_request {
  std::string id;
  std::string summary;
  std::vector<capability::capability_request> capabilities;
  std::map<std::string, std::string> metadata;
};

struct approval_decision {
  approval_decision_kind kind { approval_decision_kind::denied };
  approval_scope scope { approval_scope::once };
  std::string reason;
  std::map<std::string, std::string> metadata;
};

[[nodiscard]] inline std::string to_string(approval_decision_kind kind) {
  switch (kind) {
    case approval_decision_kind::approved:
      return "approved";
    case approval_decision_kind::denied:
      return "denied";
    case approval_decision_kind::needs_manual_review:
      return "needs_manual_review";
  }
  return "unknown";
}

[[nodiscard]] inline std::string to_string(approval_scope scope) {
  switch (scope) {
    case approval_scope::once:
      return "once";
    case approval_scope::session:
      return "session";
    case approval_scope::workspace:
      return "workspace";
  }
  return "unknown";
}

} // namespace wuwe::agent::approval

#endif // WUWE_AGENT_APPROVAL_APPROVAL_HPP
