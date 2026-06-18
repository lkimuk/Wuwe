#ifndef WUWE_AGENT_EXECUTION_EXECUTION_RUNTIME_HPP
#define WUWE_AGENT_EXECUTION_EXECUTION_RUNTIME_HPP

#include <atomic>
#include <map>
#include <memory>
#include <stop_token>
#include <string>

#include <wuwe/agent/approval/approval_service.hpp>
#include <wuwe/agent/audit/audit_sink.hpp>
#include <wuwe/agent/execution/execution_backend.hpp>
#include <wuwe/agent/execution/execution_policy.hpp>

namespace wuwe::agent::execution {

class execution_runtime {
public:
  execution_runtime(
    std::unique_ptr<execution_backend> backend,
    execution_policy policy,
    audit::audit_sink* audit = nullptr,
    approval::approval_service* approvals = nullptr);

  execution_runtime(const execution_runtime&) = delete;
  execution_runtime& operator=(const execution_runtime&) = delete;
  execution_runtime(execution_runtime&&) = delete;
  execution_runtime& operator=(execution_runtime&&) = delete;

  [[nodiscard]] execution_result run(
    execution_request request,
    std::stop_token stop_token = {});

  [[nodiscard]] const execution_policy& policy() const noexcept;
  [[nodiscard]] const execution_backend* backend() const noexcept;

  void audit_tool_rejection(
    const std::string& event_name,
    const std::string& tool_name,
    const std::string& reason,
    const std::map<std::string, std::string>& attributes = {});

private:
  std::unique_ptr<execution_backend> backend_;
  execution_policy policy_;
  audit::audit_sink* audit_;
  approval::approval_service* approvals_;
  std::atomic_size_t next_execution_id_ { 1 };
};

} // namespace wuwe::agent::execution

#endif // WUWE_AGENT_EXECUTION_EXECUTION_RUNTIME_HPP
