#ifndef WUWE_AGENT_APPROVAL_APPROVAL_SERVICE_HPP
#define WUWE_AGENT_APPROVAL_APPROVAL_SERVICE_HPP

#include <wuwe/agent/approval/approval.hpp>

namespace wuwe::agent::approval {

class approval_service {
public:
  virtual ~approval_service() = default;

  [[nodiscard]] virtual approval_decision decide(
    const approval_request& request) = 0;
};

class deny_all_approval_service final : public approval_service {
public:
  [[nodiscard]] approval_decision decide(
    const approval_request& request) override {
    return {
      .kind = approval_decision_kind::denied,
      .scope = approval_scope::once,
      .reason = request.summary.empty() ? "approval denied" : request.summary,
    };
  }
};

class allow_all_approval_service final : public approval_service {
public:
  [[nodiscard]] approval_decision decide(
    const approval_request&) override {
    return {
      .kind = approval_decision_kind::approved,
      .scope = approval_scope::once,
      .reason = "approved",
    };
  }
};

} // namespace wuwe::agent::approval

#endif // WUWE_AGENT_APPROVAL_APPROVAL_SERVICE_HPP
