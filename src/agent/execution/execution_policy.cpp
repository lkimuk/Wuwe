#include <wuwe/agent/execution/execution_policy.hpp>

#include <algorithm>
#include <sstream>
#include <string_view>
#include <utility>

namespace wuwe::agent::execution {
namespace {

bool contains_language(
  const std::vector<execution_language>& languages,
  execution_language language) {
  return std::find(languages.begin(), languages.end(), language) != languages.end();
}

bool contains_nul(std::string_view text) {
  return text.find('\0') != std::string_view::npos;
}

std::string validate_environment(const std::map<std::string, std::string>& env) {
  for (const auto& [key, value] : env) {
    if (key.empty()) {
      return "environment variable name must not be empty";
    }
    if (key.find('=') != std::string::npos) {
      return "environment variable name must not contain '=': " + key;
    }
    if (contains_nul(key) || contains_nul(value)) {
      return "environment variable must not contain NUL bytes: " + key;
    }
  }
  return {};
}

std::string capability_summary(execution_language language) {
  return "Run " + to_string(language) + " code";
}

capability::capability_request make_capability(
  std::string name,
  capability::capability_risk_level risk,
  std::string summary,
  const execution_request& request,
  const std::string& execution_id) {
  return {
    .name = std::move(name),
    .risk = risk,
    .summary = std::move(summary),
    .tool_name = request.metadata.contains("tool_name")
                   ? request.metadata.at("tool_name")
                   : std::string(),
    .trace_id = request.metadata.contains("trace_id")
                  ? request.metadata.at("trace_id")
                  : std::string(),
    .subject_id = execution_id,
  };
}

void clamp_limits(execution_limits& limits, const execution_limits& max_limits) {
  if (max_limits.timeout.count() > 0 &&
      (limits.timeout.count() <= 0 || limits.timeout > max_limits.timeout)) {
    limits.timeout = max_limits.timeout;
  }
  if (max_limits.max_stdout_bytes > 0 &&
      (limits.max_stdout_bytes == 0 || limits.max_stdout_bytes > max_limits.max_stdout_bytes)) {
    limits.max_stdout_bytes = max_limits.max_stdout_bytes;
  }
  if (max_limits.max_stderr_bytes > 0 &&
      (limits.max_stderr_bytes == 0 || limits.max_stderr_bytes > max_limits.max_stderr_bytes)) {
    limits.max_stderr_bytes = max_limits.max_stderr_bytes;
  }
  if (max_limits.max_code_bytes > 0 &&
      (limits.max_code_bytes == 0 || limits.max_code_bytes > max_limits.max_code_bytes)) {
    limits.max_code_bytes = max_limits.max_code_bytes;
  }
  if (max_limits.max_stdin_bytes > 0 &&
      (limits.max_stdin_bytes == 0 || limits.max_stdin_bytes > max_limits.max_stdin_bytes)) {
    limits.max_stdin_bytes = max_limits.max_stdin_bytes;
  }
  if (max_limits.max_total_input_bytes > 0 &&
      (limits.max_total_input_bytes == 0 ||
        limits.max_total_input_bytes > max_limits.max_total_input_bytes)) {
    limits.max_total_input_bytes = max_limits.max_total_input_bytes;
  }
}

bool exceeds_total_input_limit(
  std::size_t code_bytes,
  std::size_t stdin_bytes,
  std::size_t max_total_input_bytes) {
  if (max_total_input_bytes == 0) {
    return false;
  }
  if (code_bytes > max_total_input_bytes) {
    return true;
  }
  return stdin_bytes > max_total_input_bytes - code_bytes;
}

void add_input_limit_metadata(
  capability::capability_policy_result& result,
  const execution_request& request) {
  result.metadata["denial_kind"] = "input_limit";
  result.metadata["code_bytes"] = std::to_string(request.code.size());
  result.metadata["stdin_bytes"] = std::to_string(request.stdin_text.size());
  result.metadata["max_code_bytes"] = std::to_string(request.limits.max_code_bytes);
  result.metadata["max_stdin_bytes"] = std::to_string(request.limits.max_stdin_bytes);
  result.metadata["max_total_input_bytes"] =
    std::to_string(request.limits.max_total_input_bytes);
}

} // namespace

execution_policy_evaluation evaluate_execution_policy(
  execution_request request,
  const execution_policy& policy,
  std::string execution_id) {
  if (execution_id.empty()) {
    execution_id = request.metadata.contains("execution_id")
                     ? request.metadata.at("execution_id")
                     : std::string();
  }

  request.metadata["execution_id"] = execution_id;
  request.env = policy.allowed_env;
  if (request.workdir.empty()) {
    request.workdir = policy.default_workdir;
  }
  const auto requested_limits = request.limits;
  clamp_limits(request.limits, policy.max_limits);
  const auto environment_error = validate_environment(request.env);

  std::vector<capability::capability_request> capabilities;
  capabilities.push_back(make_capability(
    capability::names::process_python,
    capability::capability_risk_level::medium,
    capability_summary(request.language),
    request,
    execution_id));

  if (request.use_shell) {
    capabilities.push_back(make_capability(
      capability::names::process_shell,
      capability::capability_risk_level::critical,
      "Run code through a system shell",
      request,
      execution_id));
  }
  if (policy.allow_network) {
    capabilities.push_back(make_capability(
      capability::names::network_outbound,
      capability::capability_risk_level::high,
      "Allow outbound network access during execution",
      request,
      execution_id));
  }
  if (policy.allow_file_write) {
    capabilities.push_back(make_capability(
      capability::names::filesystem_write,
      capability::capability_risk_level::high,
      "Allow file writes during execution",
      request,
      execution_id));
  }

  capability::capability_policy_result result {
    .decision = capability::capability_policy_decision::allow,
    .capabilities = capabilities,
  };

  if (!contains_language(policy.allowed_languages, request.language)) {
    result.decision = capability::capability_policy_decision::deny;
    result.reason = "execution language is not allowed: " + to_string(request.language);
  }
  else if (!environment_error.empty()) {
    result.decision = capability::capability_policy_decision::deny;
    result.reason = "invalid execution environment: " + environment_error;
  }
  else if (request.limits.max_code_bytes > 0 &&
           request.code.size() > request.limits.max_code_bytes) {
    result.decision = capability::capability_policy_decision::deny;
    result.reason = "Execution denied: code is too large. code_bytes=" +
                    std::to_string(request.code.size()) +
                    " max_code_bytes=" +
                    std::to_string(request.limits.max_code_bytes) +
                    ". Use stdin_text for bulk input, or reduce the script.";
  }
  else if (request.limits.max_stdin_bytes > 0 &&
           request.stdin_text.size() > request.limits.max_stdin_bytes) {
    result.decision = capability::capability_policy_decision::deny;
    result.reason = "Execution denied: stdin_text is too large. stdin_bytes=" +
                    std::to_string(request.stdin_text.size()) +
                    " max_stdin_bytes=" +
                    std::to_string(request.limits.max_stdin_bytes) +
                    ". Pass a smaller selected input or use a heavier approved workflow.";
  }
  else if (exceeds_total_input_limit(
             request.code.size(),
             request.stdin_text.size(),
             request.limits.max_total_input_bytes)) {
    result.decision = capability::capability_policy_decision::deny;
    result.reason = "Execution denied: total input is too large. code_bytes=" +
                    std::to_string(request.code.size()) +
                    " stdin_bytes=" +
                    std::to_string(request.stdin_text.size()) +
                    " max_total_input_bytes=" +
                    std::to_string(request.limits.max_total_input_bytes) +
                    ". Reduce the script or selected input.";
  }
  else if (request.use_shell && !policy.allow_shell) {
    result.decision = capability::capability_policy_decision::deny;
    result.reason = "shell execution is not allowed";
  }
  else if (request.use_shell && policy.require_approval_for_shell) {
    result.decision = capability::capability_policy_decision::require_approval;
    result.reason = "shell execution requires approval";
  }
  else if (policy.allow_network && policy.require_approval_for_network) {
    result.decision = capability::capability_policy_decision::require_approval;
    result.reason = "network access requires approval";
  }
  else if (policy.allow_file_write && policy.require_approval_for_file_write) {
    result.decision = capability::capability_policy_decision::require_approval;
    result.reason = "file write access requires approval";
  }

  result.metadata = {
    { "execution_id", execution_id },
    { "language", to_string(request.language) },
    { "timeout_ms", std::to_string(request.limits.timeout.count()) },
    { "max_stdout_bytes", std::to_string(request.limits.max_stdout_bytes) },
    { "max_stderr_bytes", std::to_string(request.limits.max_stderr_bytes) },
    { "code_bytes", std::to_string(request.code.size()) },
    { "stdin_bytes", std::to_string(request.stdin_text.size()) },
    { "max_code_bytes", std::to_string(request.limits.max_code_bytes) },
    { "max_stdin_bytes", std::to_string(request.limits.max_stdin_bytes) },
    { "max_total_input_bytes", std::to_string(request.limits.max_total_input_bytes) },
  };
  if (!request.workdir.empty()) {
    result.metadata["workdir"] = request.workdir.string();
  }
  if (requested_limits.timeout != request.limits.timeout) {
    result.metadata["timeout_clamped"] = "true";
    result.metadata["requested_timeout_ms"] =
      std::to_string(requested_limits.timeout.count());
  }
  if (requested_limits.max_stdout_bytes != request.limits.max_stdout_bytes) {
    result.metadata["max_stdout_bytes_clamped"] = "true";
    result.metadata["requested_max_stdout_bytes"] =
      std::to_string(requested_limits.max_stdout_bytes);
  }
  if (requested_limits.max_stderr_bytes != request.limits.max_stderr_bytes) {
    result.metadata["max_stderr_bytes_clamped"] = "true";
    result.metadata["requested_max_stderr_bytes"] =
      std::to_string(requested_limits.max_stderr_bytes);
  }
  if (requested_limits.max_code_bytes != request.limits.max_code_bytes) {
    result.metadata["max_code_bytes_clamped"] = "true";
    result.metadata["requested_max_code_bytes"] =
      std::to_string(requested_limits.max_code_bytes);
  }
  if (requested_limits.max_stdin_bytes != request.limits.max_stdin_bytes) {
    result.metadata["max_stdin_bytes_clamped"] = "true";
    result.metadata["requested_max_stdin_bytes"] =
      std::to_string(requested_limits.max_stdin_bytes);
  }
  if (requested_limits.max_total_input_bytes !=
      request.limits.max_total_input_bytes) {
    result.metadata["max_total_input_bytes_clamped"] = "true";
    result.metadata["requested_max_total_input_bytes"] =
      std::to_string(requested_limits.max_total_input_bytes);
  }
  if (result.decision == capability::capability_policy_decision::deny &&
      (result.reason.rfind("Execution denied: code is too large.", 0) == 0 ||
        result.reason.rfind("Execution denied: stdin_text is too large.", 0) == 0 ||
        result.reason.rfind("Execution denied: total input is too large.", 0) == 0)) {
    add_input_limit_metadata(result, request);
  }

  return {
    .capability_result = std::move(result),
    .normalized_request = std::move(request),
  };
}

} // namespace wuwe::agent::execution
