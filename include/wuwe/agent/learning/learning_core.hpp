#ifndef WUWE_AGENT_LEARNING_LEARNING_CORE_HPP
#define WUWE_AGENT_LEARNING_LEARNING_CORE_HPP

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <wuwe/agent/approval/approval.hpp>
#include <wuwe/agent/evaluation/evaluation_core.hpp>

namespace wuwe::agent::learning {

enum class learning_change_kind {
  prompt,
  reasoning_policy,
  model_routing,
  tool_configuration,
  workflow,
  custom,
  model,
};

enum class learning_candidate_status {
  proposed,
  evaluation_failed,
  rejected,
  accepted,
  not_selected,
  approval_required,
  approval_denied,
  activation_failed,
  activated,
  timed_out,
  cancelled,
};

enum class learning_activation_mode {
  stage_only,
  require_approval,
  trusted_automatic,
};

enum class learning_stop_reason {
  none,
  cancelled,
  timed_out,
  proposal_failed,
  invalid_configuration,
};

[[nodiscard]] inline std::string to_string(learning_change_kind value) {
  switch (value) {
    case learning_change_kind::prompt:
      return "prompt";
    case learning_change_kind::reasoning_policy:
      return "reasoning_policy";
    case learning_change_kind::model_routing:
      return "model_routing";
    case learning_change_kind::tool_configuration:
      return "tool_configuration";
    case learning_change_kind::workflow:
      return "workflow";
    case learning_change_kind::custom:
      return "custom";
    case learning_change_kind::model:
      return "model";
  }
  return "unknown";
}

[[nodiscard]] inline std::string to_string(learning_candidate_status value) {
  switch (value) {
    case learning_candidate_status::proposed:
      return "proposed";
    case learning_candidate_status::evaluation_failed:
      return "evaluation_failed";
    case learning_candidate_status::rejected:
      return "rejected";
    case learning_candidate_status::accepted:
      return "accepted";
    case learning_candidate_status::not_selected:
      return "not_selected";
    case learning_candidate_status::approval_required:
      return "approval_required";
    case learning_candidate_status::approval_denied:
      return "approval_denied";
    case learning_candidate_status::activation_failed:
      return "activation_failed";
    case learning_candidate_status::activated:
      return "activated";
    case learning_candidate_status::timed_out:
      return "timed_out";
    case learning_candidate_status::cancelled:
      return "cancelled";
  }
  return "unknown";
}

[[nodiscard]] inline std::string to_string(learning_activation_mode value) {
  switch (value) {
    case learning_activation_mode::stage_only:
      return "stage_only";
    case learning_activation_mode::require_approval:
      return "require_approval";
    case learning_activation_mode::trusted_automatic:
      return "trusted_automatic";
  }
  return "unknown";
}

[[nodiscard]] inline std::string to_string(learning_stop_reason value) {
  switch (value) {
    case learning_stop_reason::none:
      return "none";
    case learning_stop_reason::cancelled:
      return "cancelled";
    case learning_stop_reason::timed_out:
      return "timed_out";
    case learning_stop_reason::proposal_failed:
      return "proposal_failed";
    case learning_stop_reason::invalid_configuration:
      return "invalid_configuration";
  }
  return "unknown";
}

struct learning_context {
  std::stop_token stop_token;
  std::optional<std::chrono::steady_clock::time_point> deadline;

  [[nodiscard]] bool cancellation_requested() const noexcept {
    return stop_token.stop_requested();
  }

  [[nodiscard]] bool deadline_reached() const noexcept {
    return deadline && std::chrono::steady_clock::now() >= *deadline;
  }

  [[nodiscard]] std::chrono::milliseconds remaining_time() const noexcept {
    if (!deadline)
      return std::chrono::milliseconds::max();
    const auto now = std::chrono::steady_clock::now();
    if (now >= *deadline)
      return std::chrono::milliseconds { 0 };
    return std::chrono::duration_cast<std::chrono::milliseconds>(*deadline - now);
  }
};

struct learning_request {
  std::string id;
  std::string goal;
  std::string target;
  std::string baseline_version;
  nlohmann::json context = nlohmann::json::object();
  std::map<std::string, std::string> metadata;
};

struct learning_candidate {
  std::string id;
  learning_change_kind kind { learning_change_kind::custom };
  std::string target;
  std::string parent_version;
  std::string proposed_version;
  nlohmann::json artifact = nlohmann::json::object();
  std::string rationale;
  std::map<std::string, std::string> metadata;
};

struct learning_evaluation {
  bool passed { false };
  double baseline_score { 0.0 };
  double candidate_score { 0.0 };
  double baseline_pass_rate { 0.0 };
  double candidate_pass_rate { 0.0 };
  std::size_t regression_count { 0 };
  std::vector<std::string> regressions;
  std::vector<std::string> evidence;
  nlohmann::json baseline_report;
  nlohmann::json candidate_report;

  [[nodiscard]] double score_improvement() const noexcept {
    return candidate_score - baseline_score;
  }

  [[nodiscard]] double pass_rate_improvement() const noexcept {
    return candidate_pass_rate - baseline_pass_rate;
  }
};

struct learning_activation_result {
  bool activated { false };
  std::string active_version;
  std::string previous_version;
  std::string rollback_token;
  std::string error;
  std::map<std::string, std::string> metadata;
};

struct learning_record {
  std::string id;
  std::string run_id;
  learning_candidate candidate;
  learning_candidate_status status { learning_candidate_status::proposed };
  std::optional<learning_evaluation> evaluation;
  std::optional<approval::approval_decision> approval;
  std::optional<learning_activation_result> activation;
  std::string error;
  bool detached { false };
  std::chrono::system_clock::time_point created_at { std::chrono::system_clock::now() };
  std::chrono::system_clock::time_point updated_at { std::chrono::system_clock::now() };
  std::map<std::string, std::string> metadata;
};

struct learning_run_result {
  bool completed { false };
  std::string run_id;
  std::vector<learning_record> records;
  learning_stop_reason stop_reason { learning_stop_reason::none };
  std::string error;
  std::size_t proposed_count { 0 };
  std::size_t truncated_count { 0 };
  std::size_t evaluated_count { 0 };
  std::size_t accepted_count { 0 };
  std::size_t rejected_count { 0 };
  std::size_t approval_required_count { 0 };
  std::size_t approval_denied_count { 0 };
  std::size_t activated_count { 0 };
  std::size_t failed_count { 0 };
  std::size_t detached_count { 0 };
  std::size_t telemetry_error_count { 0 };
  std::chrono::milliseconds elapsed { 0 };

  [[nodiscard]] explicit operator bool() const noexcept {
    return completed && stop_reason == learning_stop_reason::none;
  }
};

inline std::string make_learning_id(const char* prefix) {
  static std::atomic<std::uint64_t> next { 1 };
  const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
    std::chrono::system_clock::now().time_since_epoch())
                     .count();
  return std::string(prefix) + "-" + std::to_string(now) + "-" +
         std::to_string(next.fetch_add(1, std::memory_order_relaxed));
}

inline learning_evaluation compare_evaluation_suites(
  const evaluation::evaluation_suite_result& baseline,
  const evaluation::evaluation_suite_result& candidate) {
  learning_evaluation output {
    .baseline_score = baseline.mean_score,
    .candidate_score = candidate.mean_score,
    .baseline_pass_rate = baseline.pass_rate,
    .candidate_pass_rate = candidate.pass_rate,
    .baseline_report = evaluation::evaluation_suite_result_to_json(baseline),
    .candidate_report = evaluation::evaluation_suite_result_to_json(candidate),
  };
  std::map<std::string, bool> baseline_cases;
  for (const auto& item : baseline.cases)
    baseline_cases[item.id] = item.passed;
  for (const auto& item : candidate.cases) {
    const auto found = baseline_cases.find(item.id);
    if (found != baseline_cases.end() && found->second && !item.passed) {
      ++output.regression_count;
      output.regressions.push_back(item.id);
    }
  }
  output.passed = candidate.total != 0;
  return output;
}

inline nlohmann::json learning_candidate_to_json(const learning_candidate& value) {
  return {
    { "id", value.id },
    { "kind", to_string(value.kind) },
    { "target", value.target },
    { "parent_version", value.parent_version },
    { "proposed_version", value.proposed_version },
    { "artifact", value.artifact },
    { "rationale", value.rationale },
    { "metadata", value.metadata },
  };
}

inline nlohmann::json learning_evaluation_to_json(const learning_evaluation& value) {
  return {
    { "passed", value.passed },
    { "baseline_score", value.baseline_score },
    { "candidate_score", value.candidate_score },
    { "score_improvement", value.score_improvement() },
    { "baseline_pass_rate", value.baseline_pass_rate },
    { "candidate_pass_rate", value.candidate_pass_rate },
    { "pass_rate_improvement", value.pass_rate_improvement() },
    { "regression_count", value.regression_count },
    { "regressions", value.regressions },
    { "evidence", value.evidence },
    { "baseline_report", value.baseline_report },
    { "candidate_report", value.candidate_report },
  };
}

inline nlohmann::json learning_record_to_json(const learning_record& value) {
  return {
    { "id", value.id },
    { "run_id", value.run_id },
    { "candidate", learning_candidate_to_json(value.candidate) },
    { "status", to_string(value.status) },
    { "evaluation", value.evaluation
        ? learning_evaluation_to_json(*value.evaluation)
        : nlohmann::json(nullptr) },
    { "approval", value.approval ? nlohmann::json {
        { "kind", approval::to_string(value.approval->kind) },
        { "scope", approval::to_string(value.approval->scope) },
        { "reason", value.approval->reason },
        { "metadata", value.approval->metadata },
      } : nlohmann::json(nullptr) },
    { "activation", value.activation ? nlohmann::json {
        { "activated", value.activation->activated },
        { "active_version", value.activation->active_version },
        { "previous_version", value.activation->previous_version },
        { "rollback_token", value.activation->rollback_token },
        { "error", value.activation->error },
        { "metadata", value.activation->metadata },
      } : nlohmann::json(nullptr) },
    { "error", value.error },
    { "detached", value.detached },
    { "metadata", value.metadata },
  };
}

inline nlohmann::json learning_run_result_to_json(const learning_run_result& value) {
  auto records = nlohmann::json::array();
  for (const auto& record : value.records)
    records.push_back(learning_record_to_json(record));
  return {
    { "completed", value.completed },
    { "run_id", value.run_id },
    { "stop_reason", to_string(value.stop_reason) },
    { "error", value.error },
    { "proposed_count", value.proposed_count },
    { "truncated_count", value.truncated_count },
    { "evaluated_count", value.evaluated_count },
    { "accepted_count", value.accepted_count },
    { "rejected_count", value.rejected_count },
    { "approval_required_count", value.approval_required_count },
    { "approval_denied_count", value.approval_denied_count },
    { "activated_count", value.activated_count },
    { "failed_count", value.failed_count },
    { "detached_count", value.detached_count },
    { "telemetry_error_count", value.telemetry_error_count },
    { "elapsed_ms", value.elapsed.count() },
    { "records", std::move(records) },
  };
}

} // namespace wuwe::agent::learning

#endif // WUWE_AGENT_LEARNING_LEARNING_CORE_HPP
