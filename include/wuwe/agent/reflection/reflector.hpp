#ifndef WUWE_AGENT_REFLECTOR_HPP
#define WUWE_AGENT_REFLECTOR_HPP

#include <algorithm>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <wuwe/agent/llm/llm_client.h>
#include <wuwe/agent/reflection/reflection_core.hpp>

namespace wuwe::agent::reflection {

class reflector {
public:
  virtual ~reflector() = default;
  virtual reflection_result reflect(const reflection_request& request) = 0;
};

struct rule_reflector_options {
  bool reject_empty_output { true };
  bool require_json_object { false };
  std::size_t min_output_chars { 0 };
  std::vector<std::string> required_substrings;
  std::vector<std::string> forbidden_substrings;
};

class reflection_rule_set {
public:
  explicit reflection_rule_set(rule_reflector_options options = {}) : options_(std::move(options)) {
  }

  std::vector<reflection_issue> evaluate(const reflection_request& request) const {
    std::vector<reflection_issue> issues;

    const auto add_issue = [&](reflection_severity severity,
                             std::string code,
                             std::string message,
                             std::string suggestion = {}) {
      issues.push_back({
        .severity = severity,
        .code = std::move(code),
        .message = std::move(message),
        .suggestion = std::move(suggestion),
      });
    };

    if (options_.reject_empty_output && request.candidate_output.empty()) {
      add_issue(reflection_severity::error, "empty_output", "candidate output is empty", "retry generation");
    }

    if (options_.min_output_chars != 0 && request.candidate_output.size() < options_.min_output_chars) {
      add_issue(reflection_severity::warning,
        "short_output",
        "candidate output is shorter than the minimum length",
        "revise with more detail");
    }

    if (options_.require_json_object && !json_object(request.candidate_output)) {
      add_issue(reflection_severity::error,
        "invalid_json_object",
        "candidate output must be a JSON object",
        "retry with valid JSON object output");
    }

    for (const auto& required : options_.required_substrings) {
      if (request.candidate_output.find(required) == std::string::npos) {
        add_issue(reflection_severity::warning,
          "missing_required_text",
          "candidate output is missing required text: " + required,
          "revise to include the missing content");
      }
    }

    for (const auto& forbidden : options_.forbidden_substrings) {
      if (request.candidate_output.find(forbidden) != std::string::npos) {
        add_issue(reflection_severity::critical,
          "forbidden_text",
          "candidate output contains forbidden text: " + forbidden,
          "block or escalate this output");
      }
    }

    return issues;
  }

  static double score_for(const std::vector<reflection_issue>& issues) {
    double score = 1.0;
    for (const auto& issue : issues) {
      switch (issue.severity) {
      case reflection_severity::info:
        score -= 0.05;
        break;
      case reflection_severity::warning:
        score -= 0.20;
        break;
      case reflection_severity::error:
        score -= 0.45;
        break;
      case reflection_severity::critical:
        score -= 0.80;
        break;
      }
    }
    return (std::max)(0.0, score);
  }

  static reflection_action action_for(const std::vector<reflection_issue>& issues) {
    reflection_action action = reflection_action::revise;
    for (const auto& issue : issues) {
      if (issue.severity == reflection_severity::critical) {
        return reflection_action::escalate;
      }
      if (issue.severity == reflection_severity::error) {
        action = max_action(action, reflection_action::retry);
      }
    }
    return action;
  }

private:
  static bool json_object(const std::string& value) {
    try {
      return nlohmann::json::parse(value).is_object();
    }
    catch (...) {
      return false;
    }
  }

  rule_reflector_options options_;
};

class rule_reflector final : public reflector {
public:
  explicit rule_reflector(rule_reflector_options options = {}) : rules_(std::move(options)) {
  }

  reflection_result reflect(const reflection_request& request) override {
    reflection_result result = reflection_result::pass();
    result.metadata["reflector"] = "rule";
    result.issues = rules_.evaluate(request);

    if (!result.issues.empty()) {
      result.passed = false;
      result.score = reflection_rule_set::score_for(result.issues);
      result.recommended_action = reflection_rule_set::action_for(result.issues);
    }
    return result;
  }

private:
  reflection_rule_set rules_;
};

struct llm_reflector_options {
  std::string model;
  double temperature { 0.0 };
};

class llm_reflector final : public reflector {
public:
  explicit llm_reflector(llm_client& client, llm_reflector_options options = {})
      : client_(client), options_(std::move(options)) {
  }

  reflection_result reflect(const reflection_request& request) override {
    llm_request llm_request;
    llm_request.model = options_.model;
    llm_request.temperature = options_.temperature;
    llm_request.response_format = "json_object";
    llm_request.messages.push_back({
      .role = "system",
      .content =
        "You are a production reflection evaluator. Return only JSON with passed, score, "
        "recommended_action, issues, revised_output, and metadata. recommended_action must be "
        "one of pass, revise, retry, replan, block, escalate.",
    });
    llm_request.messages.push_back({
      .role = "user",
      .content = prompt(request),
    });

    const auto response = client_.complete(llm_request);
    if (!response) {
      throw std::runtime_error("llm reflector failed: " + response.error_code.message());
    }
    return reflection_result_normalizer(request.rubric)
      .normalize(reflection_codec::result_from_json_string(response.content));
  }

private:
  static std::string prompt(const reflection_request& request) {
    std::ostringstream out;
    out << "Task:\n" << request.task << "\n\n"
        << "Subject type: " << request.subject_type << "\n\n"
        << "Original input:\n" << request.original_input << "\n\n"
        << "Candidate output:\n" << request.candidate_output << "\n\n";
    if (!request.context.empty()) {
      out << "Context:\n" << request.context << "\n\n";
    }
    out << "Rubric:\n" << reflection_codec::rubric_to_json(request.rubric).dump() << "\n\n"
        << "Return schema:\n"
        << "{\"passed\":true,\"score\":1.0,\"recommended_action\":\"pass\","
           "\"issues\":[{\"severity\":\"warning\",\"code\":\"...\",\"message\":\"...\","
           "\"evidence\":\"...\",\"suggestion\":\"...\",\"metadata\":{}}],"
           "\"revised_output\":\"\",\"metadata\":{}}\n";
    return out.str();
  }

  llm_client& client_;
  llm_reflector_options options_;
};

class composite_reflector final : public reflector {
public:
  composite_reflector& add(std::shared_ptr<reflector> item) {
    reflectors_.push_back(std::move(item));
    return *this;
  }

  reflection_result reflect(const reflection_request& request) override {
    reflection_result_merger merger;
    for (const auto& item : reflectors_) {
      merger.add(item->reflect(request));
    }
    return merger.finish();
  }

private:
  std::vector<std::shared_ptr<reflector>> reflectors_;
};

} // namespace wuwe::agent::reflection

#endif // WUWE_AGENT_REFLECTOR_HPP
