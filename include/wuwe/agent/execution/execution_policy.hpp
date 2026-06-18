#ifndef WUWE_AGENT_EXECUTION_EXECUTION_POLICY_HPP
#define WUWE_AGENT_EXECUTION_EXECUTION_POLICY_HPP

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <wuwe/agent/capability/capability.hpp>
#include <wuwe/agent/capability/capability_policy.hpp>
#include <wuwe/agent/execution/execution_core.hpp>

namespace wuwe::agent::execution {

struct execution_policy {
  std::vector<execution_language> allowed_languages {
    execution_language::python
  };
  std::filesystem::path default_workdir;
  std::vector<std::filesystem::path> readable_roots;
  std::vector<std::filesystem::path> writable_roots;
  execution_limits max_limits;
  bool allow_network { false };
  bool allow_file_read { false };
  bool allow_file_write { false };
  bool allow_shell { false };
  bool require_approval_for_network { true };
  bool require_approval_for_file_write { true };
  bool require_approval_for_shell { true };
  std::map<std::string, std::string> allowed_env;
};

struct execution_policy_evaluation {
  capability::capability_policy_result capability_result;
  execution_request normalized_request;
};

[[nodiscard]] execution_policy_evaluation evaluate_execution_policy(
  execution_request request,
  const execution_policy& policy,
  std::string execution_id = {});

} // namespace wuwe::agent::execution

#endif // WUWE_AGENT_EXECUTION_EXECUTION_POLICY_HPP
