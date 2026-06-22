#include <wuwe/agent/execution/execution_runtime.hpp>

#include <chrono>
#include <exception>
#include <sstream>
#include <utility>

namespace wuwe::agent::execution {
namespace {

std::string make_execution_id(std::size_t id) {
  return "execution-" + std::to_string(id);
}

audit::audit_event make_audit_event(
  const std::string& name,
  const std::string& execution_id,
  audit::audit_event_outcome outcome,
  const execution_request& request,
  std::chrono::milliseconds elapsed = std::chrono::milliseconds { 0 }) {
  audit::audit_event event;
  event.module = "execution";
  event.name = name;
  event.id = execution_id;
  event.subject_id = execution_id;
  event.outcome = outcome;
  event.elapsed = elapsed;
  event.attributes = {
    { "language", to_string(request.language) },
    { "timeout_ms", std::to_string(request.limits.timeout.count()) },
    { "max_stdout_bytes", std::to_string(request.limits.max_stdout_bytes) },
    { "max_stderr_bytes", std::to_string(request.limits.max_stderr_bytes) },
    { "max_process_count", std::to_string(request.limits.max_process_count) },
    { "max_memory_bytes", std::to_string(request.limits.max_memory_bytes) },
    { "max_cpu_time_ms", std::to_string(request.limits.max_cpu_time.count()) },
    { "use_shell", request.use_shell ? "true" : "false" },
  };
  if (!request.workdir.empty()) {
    event.attributes["workdir"] = request.workdir.string();
  }
  if (const auto trace = request.metadata.find("trace_id"); trace != request.metadata.end()) {
    event.trace_id = trace->second;
  }
  return event;
}

void add_backend_attributes(
  audit::audit_event& event,
  const execution_backend* backend) {
  if (backend == nullptr) {
    event.attributes["backend"] = "none";
    return;
  }

  const auto info = backend->info();
  event.attributes["backend"] = info.name;
  event.attributes["isolation"] = sandbox::to_string(info.isolation);
  event.attributes["shell_execution_enforcement"] =
    sandbox::to_string(info.enforcement.shell_execution);
  event.attributes["timeout_enforcement"] =
    sandbox::to_string(info.enforcement.timeout);
  event.attributes["cancellation_enforcement"] =
    sandbox::to_string(info.enforcement.cancellation);
  event.attributes["stdout_limit_enforcement"] =
    sandbox::to_string(info.enforcement.stdout_limit);
  event.attributes["stderr_limit_enforcement"] =
    sandbox::to_string(info.enforcement.stderr_limit);
  event.attributes["environment_allowlist_enforcement"] =
    sandbox::to_string(info.enforcement.environment_allowlist);
  event.attributes["working_directory_enforcement"] =
    sandbox::to_string(info.enforcement.working_directory);
  event.attributes["process_tree_cleanup_enforcement"] =
    sandbox::to_string(info.enforcement.process_tree_cleanup);
  event.attributes["process_count_limit_enforcement"] =
    sandbox::to_string(info.enforcement.process_count_limit);
  event.attributes["cpu_time_limit_enforcement"] =
    sandbox::to_string(info.enforcement.cpu_time_limit);
  event.attributes["memory_limit_enforcement"] =
    sandbox::to_string(info.enforcement.memory_limit);
  event.attributes["file_read_deny_enforcement"] =
    sandbox::to_string(info.enforcement.filesystem_read_deny);
  event.attributes["file_write_deny_enforcement"] =
    sandbox::to_string(info.enforcement.filesystem_write_deny);
  event.attributes["network_deny_enforcement"] =
    sandbox::to_string(info.enforcement.network_deny);
}

std::string policy_event_name(
  const capability::capability_policy_result& result) {
  const auto denial_kind = result.metadata.find("denial_kind");
  if (denial_kind != result.metadata.end() && denial_kind->second == "input_limit") {
    return "input_limit";
  }
  return "policy_evaluated";
}

execution_result denied_result(
  execution_termination_reason reason,
  std::string message,
  const capability::capability_policy_result& policy_result) {
  return {
    .termination_reason = reason,
    .error_message = std::move(message),
    .metadata = policy_result.metadata,
  };
}

audit::audit_event_outcome outcome_for_result(const execution_result& result) {
  switch (result.termination_reason) {
    case execution_termination_reason::exited:
      return audit::audit_event_outcome::completed;
    case execution_termination_reason::timeout:
      return audit::audit_event_outcome::timed_out;
    case execution_termination_reason::cancelled:
      return audit::audit_event_outcome::cancelled;
    case execution_termination_reason::policy_denied:
    case execution_termination_reason::approval_denied:
      return audit::audit_event_outcome::denied;
    case execution_termination_reason::launch_failed:
    case execution_termination_reason::backend_error:
      return audit::audit_event_outcome::failed;
  }
  return audit::audit_event_outcome::failed;
}

} // namespace

execution_runtime::execution_runtime(
  std::unique_ptr<execution_backend> backend,
  execution_policy policy,
  audit::audit_sink* audit,
  approval::approval_service* approvals)
    : backend_(std::move(backend)),
      policy_(std::move(policy)),
      audit_(audit),
      approvals_(approvals) {
}

execution_result execution_runtime::run(
  execution_request request,
  std::stop_token stop_token) {
  const auto execution_id = make_execution_id(next_execution_id_.fetch_add(1));
  request.metadata["execution_id"] = execution_id;

  const auto evaluation =
    evaluate_execution_policy(std::move(request), policy_, execution_id);
  auto normalized = evaluation.normalized_request;

  if (audit_) {
    auto event = make_audit_event(
      policy_event_name(evaluation.capability_result),
      execution_id,
      evaluation.capability_result.decision ==
          capability::capability_policy_decision::deny
        ? audit::audit_event_outcome::denied
        : audit::audit_event_outcome::allowed,
      normalized);
    for (const auto& [key, value] : evaluation.capability_result.metadata) {
      event.attributes.try_emplace(key, value);
    }
    audit_->publish(event);
  }

  if (evaluation.capability_result.decision ==
      capability::capability_policy_decision::deny) {
    return denied_result(
      execution_termination_reason::policy_denied,
      evaluation.capability_result.reason,
      evaluation.capability_result);
  }

  if (evaluation.capability_result.decision ==
      capability::capability_policy_decision::require_approval) {
    if (!approvals_) {
      if (audit_) {
        audit_->publish(make_audit_event(
          "approval_missing",
          execution_id,
          audit::audit_event_outcome::denied,
          normalized));
      }
      return denied_result(
        execution_termination_reason::approval_denied,
        "approval required but no approval service is configured",
        evaluation.capability_result);
    }

    approval::approval_request approval_request {
      .id = execution_id,
      .summary = evaluation.capability_result.reason,
      .capabilities = evaluation.capability_result.capabilities,
      .metadata = evaluation.capability_result.metadata,
    };
    const auto decision = approvals_->decide(approval_request);
    if (decision.kind != approval::approval_decision_kind::approved) {
      if (audit_) {
        auto event = make_audit_event(
          "approval_denied",
          execution_id,
          audit::audit_event_outcome::denied,
          normalized);
        event.attributes["approval_reason"] = decision.reason;
        audit_->publish(event);
      }
      return denied_result(
        execution_termination_reason::approval_denied,
        decision.reason.empty() ? "approval denied" : decision.reason,
        evaluation.capability_result);
    }

    if (audit_) {
      auto event = make_audit_event(
        "approval_approved",
        execution_id,
        audit::audit_event_outcome::approved,
        normalized);
      event.attributes["approval_scope"] = approval::to_string(decision.scope);
      audit_->publish(event);
    }
  }

  if (!backend_) {
    execution_result result {
      .termination_reason = execution_termination_reason::backend_error,
      .error_message = "execution backend is not configured",
      .metadata = evaluation.capability_result.metadata,
    };
    if (audit_) {
      audit_->publish(make_audit_event(
        "backend_missing",
        execution_id,
        audit::audit_event_outcome::failed,
        normalized));
    }
    return result;
  }

  if (audit_) {
    audit_->publish(make_audit_event(
      "execution_started",
      execution_id,
      audit::audit_event_outcome::started,
      normalized));
  }

  const auto started = std::chrono::steady_clock::now();
  execution_result result;
  try {
    result = backend_->run(normalized, stop_token);
  }
  catch (const std::exception& ex) {
    result = {
      .termination_reason = execution_termination_reason::backend_error,
      .error_message = ex.what(),
    };
  }
  catch (...) {
    result = {
      .termination_reason = execution_termination_reason::backend_error,
      .error_message = "execution backend threw an unknown exception",
    };
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now() - started);
  if (result.elapsed.count() == 0) {
    result.elapsed = elapsed;
  }
  for (const auto& [key, value] : evaluation.capability_result.metadata) {
    result.metadata.try_emplace(key, value);
  }

  if (audit_) {
    auto event = make_audit_event(
      "execution_finished",
      execution_id,
      outcome_for_result(result),
      normalized,
      result.elapsed);
    event.attributes["termination_reason"] = to_string(result.termination_reason);
    event.attributes["stdout_bytes"] = std::to_string(result.stdout_text.size());
    event.attributes["stderr_bytes"] = std::to_string(result.stderr_text.size());
    event.attributes["stdout_truncated"] = result.stdout_truncated ? "true" : "false";
    event.attributes["stderr_truncated"] = result.stderr_truncated ? "true" : "false";
    if (result.exit_code.has_value()) {
      event.attributes["exit_code"] = std::to_string(*result.exit_code);
    }
    if (!result.error_message.empty()) {
      event.attributes["error"] = result.error_message;
    }
    add_backend_attributes(event, backend_.get());
    audit_->publish(event);
  }

  return result;
}

void execution_runtime::audit_tool_rejection(
  const std::string& event_name,
  const std::string& tool_name,
  const std::string& reason,
  const std::map<std::string, std::string>& attributes) {
  if (!audit_) {
    return;
  }

  const auto rejection_id = make_execution_id(next_execution_id_.fetch_add(1));

  audit::audit_event event;
  event.module = "execution";
  event.name = event_name;
  event.id = rejection_id;
  event.subject_id = rejection_id;
  event.outcome = audit::audit_event_outcome::denied;
  event.attributes = attributes;
  event.attributes["execution_id"] = rejection_id;
  event.attributes["tool_name"] = tool_name;
  event.attributes["reason"] = reason;
  add_backend_attributes(event, backend_.get());
  audit_->publish(event);
}

const execution_policy& execution_runtime::policy() const noexcept {
  return policy_;
}

const execution_backend* execution_runtime::backend() const noexcept {
  return backend_.get();
}

} // namespace wuwe::agent::execution
