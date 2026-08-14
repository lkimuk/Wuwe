#ifndef WUWE_AGENT_TRAINING_TRAINING_POLICY_HPP
#define WUWE_AGENT_TRAINING_TRAINING_POLICY_HPP

#include <string>

#include <wuwe/agent/capability/capability.hpp>
#include <wuwe/agent/capability/capability_policy.hpp>
#include <wuwe/agent/training/training_context.hpp>

namespace wuwe::agent::training {

enum class training_operation { submit, refresh, cancel, resume, reconcile };

struct training_policy {
  bool allow_submit { true };
  bool allow_refresh { true };
  bool allow_cancel { true };
  bool allow_resume { true };
  bool allow_reconcile { true };
  bool require_approval_for_submit { true };
  bool require_approval_for_cancel { false };
  bool require_approval_for_resume { true };
};

[[nodiscard]] inline training_policy trusted_training_policy() noexcept {
  training_policy policy;
  policy.require_approval_for_submit = false;
  policy.require_approval_for_resume = false;
  return policy;
}

[[nodiscard]] inline const char* capability_name(training_operation operation) noexcept {
  switch (operation) {
    case training_operation::submit:
      return capability::names::training_submit;
    case training_operation::refresh:
      return capability::names::training_refresh;
    case training_operation::cancel:
      return capability::names::training_cancel;
    case training_operation::resume:
      return capability::names::training_resume;
    case training_operation::reconcile:
      return capability::names::training_reconcile;
  }
  return capability::names::training_refresh;
}

[[nodiscard]] inline capability::capability_policy_result evaluate_training_policy(
  training_operation operation, const training_policy& policy, const training_context& context,
  const std::string& resource) {
  const bool allowed = operation == training_operation::submit    ? policy.allow_submit
                       : operation == training_operation::refresh ? policy.allow_refresh
                       : operation == training_operation::cancel  ? policy.allow_cancel
                       : operation == training_operation::resume  ? policy.allow_resume
                                                                  : policy.allow_reconcile;
  const bool approval =
    operation == training_operation::submit   ? policy.require_approval_for_submit
    : operation == training_operation::cancel ? policy.require_approval_for_cancel
    : operation == training_operation::resume ? policy.require_approval_for_resume
                                              : false;
  capability::capability_request request {
    .name = capability_name(operation),
    .risk = operation == training_operation::refresh || operation == training_operation::reconcile
              ? capability::capability_risk_level::low
              : capability::capability_risk_level::high,
    .summary = std::string("authorize ") + capability_name(operation),
    .resources = resource.empty() ? std::vector<std::string> {} : std::vector { resource },
    .tool_name = "training_coordinator",
    .trace_id = context.trace_id,
    .subject_id = context.subject_id,
    .metadata = context.metadata,
  };
  request.metadata.insert_or_assign("tenant_id", context.tenant_id);
  request.metadata.insert_or_assign("workspace_id", context.workspace_id);
  request.metadata.insert_or_assign("request_id", context.request_id);
  auto approval_metadata = request.metadata;
  return {
    .decision = !allowed   ? capability::capability_policy_decision::deny
                : approval ? capability::capability_policy_decision::require_approval
                           : capability::capability_policy_decision::allow,
    .reason = !allowed   ? "training operation denied by policy"
              : approval ? "training operation requires approval"
                         : "allowed",
    .capabilities = { std::move(request) },
    .metadata = std::move(approval_metadata),
  };
}

} // namespace wuwe::agent::training

#endif // WUWE_AGENT_TRAINING_TRAINING_POLICY_HPP
