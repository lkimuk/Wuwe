#ifndef WUWE_AGENT_CAPABILITY_CAPABILITY_POLICY_HPP
#define WUWE_AGENT_CAPABILITY_CAPABILITY_POLICY_HPP

#include <map>
#include <string>
#include <vector>

#include <wuwe/agent/capability/capability.hpp>

namespace wuwe::agent::capability {

enum class capability_policy_decision {
  allow,
  deny,
  require_approval,
};

struct capability_policy_result {
  capability_policy_decision decision { capability_policy_decision::deny };
  std::string reason;
  std::vector<capability_request> capabilities;
  std::map<std::string, std::string> metadata;
};

[[nodiscard]] inline std::string to_string(capability_policy_decision decision) {
  switch (decision) {
    case capability_policy_decision::allow:
      return "allow";
    case capability_policy_decision::deny:
      return "deny";
    case capability_policy_decision::require_approval:
      return "require_approval";
  }
  return "unknown";
}

} // namespace wuwe::agent::capability

#endif // WUWE_AGENT_CAPABILITY_CAPABILITY_POLICY_HPP
