#ifndef WUWE_AGENT_REFLECTION_CORE_HPP
#define WUWE_AGENT_REFLECTION_CORE_HPP

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace wuwe::agent::reflection {

enum class reflection_action {
  pass,
  revise,
  retry,
  replan,
  block,
  escalate,
};

inline std::string to_string(reflection_action action) {
  switch (action) {
  case reflection_action::pass:
    return "pass";
  case reflection_action::revise:
    return "revise";
  case reflection_action::retry:
    return "retry";
  case reflection_action::replan:
    return "replan";
  case reflection_action::block:
    return "block";
  case reflection_action::escalate:
    return "escalate";
  }
  return "unknown";
}

inline std::optional<reflection_action> reflection_action_from_string(const std::string& value) {
  if (value == "pass") {
    return reflection_action::pass;
  }
  if (value == "revise") {
    return reflection_action::revise;
  }
  if (value == "retry") {
    return reflection_action::retry;
  }
  if (value == "replan") {
    return reflection_action::replan;
  }
  if (value == "block") {
    return reflection_action::block;
  }
  if (value == "escalate") {
    return reflection_action::escalate;
  }
  return std::nullopt;
}

enum class reflection_severity {
  info,
  warning,
  error,
  critical,
};

inline std::string to_string(reflection_severity severity) {
  switch (severity) {
  case reflection_severity::info:
    return "info";
  case reflection_severity::warning:
    return "warning";
  case reflection_severity::error:
    return "error";
  case reflection_severity::critical:
    return "critical";
  }
  return "unknown";
}

inline std::optional<reflection_severity> reflection_severity_from_string(
  const std::string& value) {
  if (value == "info") {
    return reflection_severity::info;
  }
  if (value == "warning") {
    return reflection_severity::warning;
  }
  if (value == "error") {
    return reflection_severity::error;
  }
  if (value == "critical") {
    return reflection_severity::critical;
  }
  return std::nullopt;
}

inline int severity_rank(reflection_severity severity) {
  switch (severity) {
  case reflection_severity::info:
    return 0;
  case reflection_severity::warning:
    return 1;
  case reflection_severity::error:
    return 2;
  case reflection_severity::critical:
    return 3;
  }
  return 0;
}

inline int action_rank(reflection_action action) {
  switch (action) {
  case reflection_action::pass:
    return 0;
  case reflection_action::revise:
    return 1;
  case reflection_action::retry:
    return 2;
  case reflection_action::replan:
    return 3;
  case reflection_action::block:
    return 4;
  case reflection_action::escalate:
    return 5;
  }
  return 0;
}

inline reflection_action max_action(reflection_action lhs, reflection_action rhs) {
  return action_rank(lhs) >= action_rank(rhs) ? lhs : rhs;
}

struct reflection_issue {
  reflection_severity severity { reflection_severity::warning };
  std::string code;
  std::string message;
  std::string evidence;
  std::string suggestion;
  std::map<std::string, std::string> metadata;
};

struct reflection_criterion {
  std::string name;
  std::string description;
  double weight { 1.0 };
  double pass_threshold { 0.75 };
  std::map<std::string, std::string> metadata;
};

struct reflection_rubric {
  std::vector<reflection_criterion> criteria;
  double pass_threshold { 0.75 };
  bool require_evidence { true };
  bool allow_revision { true };
  std::map<std::string, std::string> metadata;
};

struct reflection_request {
  std::string task;
  std::string original_input;
  std::string candidate_output;
  std::string context;
  std::string subject_type { "output" };
  reflection_rubric rubric;
  std::map<std::string, std::string> metadata;
};

struct reflection_result {
  bool passed { true };
  double score { 1.0 };
  reflection_action recommended_action { reflection_action::pass };
  std::vector<reflection_issue> issues;
  std::string revised_output;
  std::map<std::string, std::string> metadata;

  static reflection_result pass(double score = 1.0) {
    return { .passed = true, .score = score, .recommended_action = reflection_action::pass };
  }

  static reflection_result fail(
    reflection_action action,
    reflection_issue issue,
    double score = 0.0) {
    return {
      .passed = false,
      .score = score,
      .recommended_action = action,
      .issues = { std::move(issue) },
    };
  }
};

struct reflection_policy {
  double pass_threshold { 0.75 };
  double revise_threshold { 0.55 };
  double retry_threshold { 0.35 };
  bool escalate_on_critical { true };
  bool block_on_error { false };

  reflection_action action_for(const reflection_result& result) const;
};

class reflection_policy_engine {
public:
  explicit reflection_policy_engine(reflection_policy policy = {}) : policy_(policy) {
  }

  reflection_action action_for(const reflection_result& result) const {
    if (result.passed && result.score >= policy_.pass_threshold && result.issues.empty()) {
      return reflection_action::pass;
    }

    const auto worst = worst_severity(result.issues);
    if (worst && *worst == reflection_severity::critical && policy_.escalate_on_critical) {
      return reflection_action::escalate;
    }
    if (worst && *worst == reflection_severity::error && policy_.block_on_error) {
      return reflection_action::block;
    }
    if (result.recommended_action == reflection_action::replan) {
      return reflection_action::replan;
    }
    if (result.score >= policy_.revise_threshold && !result.revised_output.empty()) {
      return reflection_action::revise;
    }
    if (result.score >= policy_.retry_threshold) {
      return reflection_action::retry;
    }
    return reflection_action::block;
  }

  reflection_result apply(reflection_result result) const {
    result.recommended_action = max_action(result.recommended_action, action_for(result));
    result.passed = result.recommended_action == reflection_action::pass;
    return result;
  }

private:
  static std::optional<reflection_severity> worst_severity(
    const std::vector<reflection_issue>& issues) {
    if (issues.empty()) {
      return std::nullopt;
    }
    return std::max_element(issues.begin(), issues.end(), [](const auto& lhs, const auto& rhs) {
      return severity_rank(lhs.severity) < severity_rank(rhs.severity);
    })->severity;
  }

  reflection_policy policy_;
};

inline reflection_action action_for(const reflection_policy& policy, const reflection_result& result) {
  return reflection_policy_engine(policy).action_for(result);
}

inline reflection_action reflection_policy::action_for(const reflection_result& result) const {
  return reflection_policy_engine(*this).action_for(result);
}

class reflection_result_normalizer {
public:
  explicit reflection_result_normalizer(reflection_rubric rubric = {}) : rubric_(std::move(rubric)) {
  }

  reflection_result normalize(reflection_result result) const {
    result.score = std::clamp(result.score, 0.0, 1.0);
    if (result.passed && !result.issues.empty()) {
      result.passed = false;
    }
    if (!rubric_.allow_revision && result.recommended_action == reflection_action::revise) {
      result.recommended_action = reflection_action::retry;
      result.revised_output.clear();
    }
    return result;
  }

private:
  reflection_rubric rubric_;
};

class reflection_result_merger {
public:
  reflection_result_merger& add(reflection_result result) {
    if (!has_value_) {
      merged_ = std::move(result);
      has_value_ = true;
      return *this;
    }

    merged_.passed = merged_.passed && result.passed;
    merged_.score = (std::min)(merged_.score, result.score);
    merged_.recommended_action = max_action(merged_.recommended_action, result.recommended_action);
    if (merged_.revised_output.empty() && !result.revised_output.empty()) {
      merged_.revised_output = std::move(result.revised_output);
    }
    merged_.issues.insert(merged_.issues.end(),
      std::make_move_iterator(result.issues.begin()),
      std::make_move_iterator(result.issues.end()));
    for (auto& [key, value] : result.metadata) {
      merged_.metadata[key] = std::move(value);
    }
    return *this;
  }

  reflection_result finish() const {
    if (!has_value_) {
      return reflection_result::pass();
    }
    auto output = merged_;
    if (!output.issues.empty()) {
      output.passed = false;
      if (output.recommended_action == reflection_action::pass) {
        output.recommended_action = reflection_action::revise;
      }
    }
    return output;
  }

private:
  bool has_value_ {};
  reflection_result merged_ { reflection_result::pass() };
};

struct reflection_record {
  std::string id;
  std::chrono::system_clock::time_point created_at { std::chrono::system_clock::now() };
  reflection_request request;
  reflection_result result;
  std::map<std::string, std::string> metadata;
};

class reflection_codec {
public:
  static std::string extract_json_object(std::string content) {
    const auto first = content.find('{');
    const auto last = content.rfind('}');
    if (first == std::string::npos || last == std::string::npos || last < first) {
      return content;
    }
    return content.substr(first, last - first + 1);
  }

  static nlohmann::json issue_to_json(const reflection_issue& issue) {
    return {
      { "severity", to_string(issue.severity) },
      { "code", issue.code },
      { "message", issue.message },
      { "evidence", issue.evidence },
      { "suggestion", issue.suggestion },
      { "metadata", issue.metadata },
    };
  }

  static reflection_issue issue_from_json(const nlohmann::json& json) {
    reflection_issue issue;
    issue.severity = reflection_severity_from_string(json.value("severity", "warning"))
                       .value_or(reflection_severity::warning);
    issue.code = json.value("code", std::string {});
    issue.message = json.value("message", std::string {});
    issue.evidence = json.value("evidence", std::string {});
    issue.suggestion = json.value("suggestion", std::string {});
    issue.metadata = json.value("metadata", std::map<std::string, std::string> {});
    return issue;
  }

  static nlohmann::json result_to_json(const reflection_result& result) {
    nlohmann::json issues = nlohmann::json::array();
    for (const auto& issue : result.issues) {
      issues.push_back(issue_to_json(issue));
    }
    return {
      { "passed", result.passed },
      { "score", result.score },
      { "recommended_action", to_string(result.recommended_action) },
      { "issues", std::move(issues) },
      { "revised_output", result.revised_output },
      { "metadata", result.metadata },
    };
  }

  static reflection_result result_from_json(const nlohmann::json& json) {
    reflection_result result;
    result.passed = json.value("passed", true);
    result.score = json.value("score", result.passed ? 1.0 : 0.0);
    result.recommended_action =
      reflection_action_from_string(json.value("recommended_action", std::string("pass")))
        .value_or(result.passed ? reflection_action::pass : reflection_action::retry);
    if (json.contains("issues") && json.at("issues").is_array()) {
      for (const auto& item : json.at("issues")) {
        result.issues.push_back(issue_from_json(item));
      }
    }
    result.revised_output = json.value("revised_output", std::string {});
    result.metadata = json.value("metadata", std::map<std::string, std::string> {});
    return result;
  }

  static reflection_result result_from_json_string(const std::string& content) {
    return result_from_json(nlohmann::json::parse(extract_json_object(content)));
  }

  static nlohmann::json criterion_to_json(const reflection_criterion& criterion) {
    return {
      { "name", criterion.name },
      { "description", criterion.description },
      { "weight", criterion.weight },
      { "pass_threshold", criterion.pass_threshold },
      { "metadata", criterion.metadata },
    };
  }

  static nlohmann::json rubric_to_json(const reflection_rubric& rubric) {
    nlohmann::json criteria = nlohmann::json::array();
    for (const auto& criterion : rubric.criteria) {
      criteria.push_back(criterion_to_json(criterion));
    }
    return {
      { "criteria", std::move(criteria) },
      { "pass_threshold", rubric.pass_threshold },
      { "require_evidence", rubric.require_evidence },
      { "allow_revision", rubric.allow_revision },
      { "metadata", rubric.metadata },
    };
  }

  static nlohmann::json request_to_json(const reflection_request& request) {
    return {
      { "task", request.task },
      { "original_input", request.original_input },
      { "candidate_output", request.candidate_output },
      { "context", request.context },
      { "subject_type", request.subject_type },
      { "rubric", rubric_to_json(request.rubric) },
      { "metadata", request.metadata },
    };
  }

  static nlohmann::json record_to_json(const reflection_record& record) {
    const auto created_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      record.created_at.time_since_epoch()).count();
    return {
      { "id", record.id },
      { "created_at_unix_millis", created_at_ms },
      { "request", request_to_json(record.request) },
      { "result", result_to_json(record.result) },
      { "metadata", record.metadata },
    };
  }

  static reflection_record record_from_json(const nlohmann::json& json) {
    reflection_record record;
    record.id = json.value("id", std::string {});
    if (json.contains("created_at_unix_millis")) {
      record.created_at = std::chrono::system_clock::time_point(
        std::chrono::milliseconds(json.at("created_at_unix_millis").get<std::int64_t>()));
    }
    if (json.contains("request")) {
      const auto& req = json.at("request");
      record.request.task = req.value("task", std::string {});
      record.request.original_input = req.value("original_input", std::string {});
      record.request.candidate_output = req.value("candidate_output", std::string {});
      record.request.context = req.value("context", std::string {});
      record.request.subject_type = req.value("subject_type", std::string("output"));
      record.request.metadata = req.value("metadata", std::map<std::string, std::string> {});
    }
    if (json.contains("result")) {
      record.result = result_from_json(json.at("result"));
    }
    record.metadata = json.value("metadata", std::map<std::string, std::string> {});
    return record;
  }
};

inline reflection_result reflection_result_from_json_string(const std::string& content) {
  return reflection_codec::result_from_json_string(content);
}

inline nlohmann::json reflection_result_to_json(const reflection_result& result) {
  return reflection_codec::result_to_json(result);
}

} // namespace wuwe::agent::reflection

#endif // WUWE_AGENT_REFLECTION_CORE_HPP
