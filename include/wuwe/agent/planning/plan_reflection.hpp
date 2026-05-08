#ifndef WUWE_AGENT_PLANNING_PLAN_REFLECTION_HPP
#define WUWE_AGENT_PLANNING_PLAN_REFLECTION_HPP

#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include <wuwe/agent/planning/plan.hpp>
#include <wuwe/agent/reflection/reflection_runner.hpp>

namespace wuwe::agent::planning {

struct plan_reflection_options {
  std::shared_ptr<agent::reflection::reflection_runner> runner;
  agent::reflection::reflection_rubric rubric;
  bool reflect_completed_steps { true };
  bool reflect_failed_steps { false };
  bool reflect_blocked_steps { false };
  std::function<agent::reflection::reflection_request(
    const plan_step&,
    const plan&,
    const planning_observation&)> request_builder;
};

class plan_reflection_scope {
public:
  explicit plan_reflection_scope(const plan_reflection_options& options)
      : options_(options) {
  }

  bool contains(plan_step_status status) const {
    return (status == plan_step_status::completed && options_.reflect_completed_steps) ||
           (status == plan_step_status::failed && options_.reflect_failed_steps) ||
           (status == plan_step_status::blocked && options_.reflect_blocked_steps);
  }

private:
  const plan_reflection_options& options_;
};

class plan_reflection_metadata {
public:
  static constexpr std::string_view passed_key = "reflection_passed";
  static constexpr std::string_view score_key = "reflection_score";
  static constexpr std::string_view action_key = "reflection_action";
  static constexpr std::string_view issue_count_key = "reflection_issue_count";
  static constexpr std::string_view issue_key = "reflection_issue";

  static std::optional<agent::reflection::reflection_action> action_for(const plan_step& step) {
    const auto found = step.metadata.find(key(action_key));
    if (found == step.metadata.end()) {
      return std::nullopt;
    }
    return agent::reflection::reflection_action_from_string(found->second);
  }

  static void write(plan_step& step, const agent::reflection::reflection_result& result) {
    step.metadata[key(passed_key)] = result.passed ? "true" : "false";
    step.metadata[key(score_key)] = std::to_string(result.score);
    step.metadata[key(action_key)] = agent::reflection::to_string(result.recommended_action);
    step.metadata[key(issue_count_key)] = std::to_string(result.issues.size());
    if (result.issues.empty()) {
      step.metadata.erase(key(issue_key));
    }
    else {
      step.metadata[key(issue_key)] = result.issues.front().code;
    }
  }

private:
  static std::string key(std::string_view value) {
    return std::string(value);
  }
};

class plan_reflection_request_factory {
public:
  explicit plan_reflection_request_factory(const plan_reflection_options& options)
      : options_(options) {
  }

  agent::reflection::reflection_request build(
    const plan_step& step,
    const plan& value,
    const planning_observation& observation) const {
    auto request = options_.request_builder
                     ? options_.request_builder(step, value, observation)
                     : default_request(step, value);
    if (request.rubric.criteria.empty() && !options_.rubric.criteria.empty()) {
      request.rubric = options_.rubric;
    }
    request.metadata.try_emplace("plan_id", value.id);
    request.metadata.try_emplace("step_id", step.id);
    request.metadata.try_emplace("step_status", to_string(step.status));
    return request;
  }

private:
  agent::reflection::reflection_request default_request(
    const plan_step& step,
    const plan& value) const {
    std::ostringstream task;
    task << "Reflect the result of planning step '" << step.id << "' for goal: " << value.goal;

    return {
      .task = task.str(),
      .original_input = step_input_json(step).dump(),
      .candidate_output = step.output.empty() ? step.error : step.output,
      .context = plan_context_json(step, value).dump(),
      .subject_type = "plan_step_result",
      .rubric = options_.rubric,
      .metadata = {
        { "plan_id", value.id },
        { "step_id", step.id },
        { "step_status", to_string(step.status) },
      },
    };
  }

  static nlohmann::json step_input_json(const plan_step& step) {
    return {
      { "id", step.id },
      { "title", step.title },
      { "description", step.description },
      { "assigned_tool", step.assigned_tool ? nlohmann::json(*step.assigned_tool) : nlohmann::json(nullptr) },
      { "assigned_agent", step.assigned_agent ? nlohmann::json(*step.assigned_agent) : nlohmann::json(nullptr) },
      { "input", step.input },
      { "input_json", step.input_json.is_null() ? nlohmann::json(nullptr) : step.input_json },
    };
  }

  static nlohmann::json plan_context_json(const plan_step& step, const plan& value) {
    return {
      { "plan_id", value.id },
      { "goal", value.goal },
      { "step_count", value.steps.size() },
      { "step_id", step.id },
      { "step_status", to_string(step.status) },
    };
  }

  const plan_reflection_options& options_;
};

class plan_reflection_action_applier {
public:
  static void apply(plan_step& step, const agent::reflection::reflection_result& result) {
    using agent::reflection::reflection_action;

    plan_reflection_metadata::write(step, result);

    switch (result.recommended_action) {
    case reflection_action::pass:
      return;
    case reflection_action::revise:
      revise(step, result);
      return;
    case reflection_action::retry:
      fail(step, "reflection requested retry");
      return;
    case reflection_action::replan:
      fail(step, "reflection requested replanning");
      return;
    case reflection_action::block:
      block(step, "reflection blocked step result");
      return;
    case reflection_action::escalate:
      block(step, "reflection escalated step result");
      return;
    }
  }

private:
  static void revise(plan_step& step, const agent::reflection::reflection_result& result) {
    if (result.revised_output.empty()) {
      fail(step, "reflection requested revision but did not provide revised output");
      return;
    }
    step.output = result.revised_output;
    step.output_json = nlohmann::json();
    step.error.clear();
    step.status = plan_step_status::completed;
  }

  static void fail(plan_step& step, std::string error) {
    step.status = plan_step_status::failed;
    step.error = std::move(error);
  }

  static void block(plan_step& step, std::string error) {
    step.status = plan_step_status::blocked;
    step.error = std::move(error);
  }
};

class plan_reflection_gate {
public:
  explicit plan_reflection_gate(plan_reflection_options options = {})
      : options_(std::move(options)) {
  }

  bool enabled() const {
    return options_.runner != nullptr;
  }

  std::optional<agent::reflection::reflection_result> review(plan& value, plan_step& step) const {
    if (!enabled() || !plan_reflection_scope(options_).contains(step.status)) {
      return std::nullopt;
    }

    auto request = plan_reflection_request_factory(options_).build(
      step,
      value,
      observation_from(step, value.id));
    const auto run = options_.runner->run(std::move(request));
    plan_reflection_action_applier::apply(step, run.result);
    return run.result;
  }

  static planning_observation observation_from(const plan_step& step, const std::string& plan_id) {
    return {
      .plan_id = plan_id,
      .step_id = step.id,
      .status = step.status,
      .output = step.output,
      .error = step.error,
      .metadata = step.metadata,
    };
  }

private:
  plan_reflection_options options_;
};

} // namespace wuwe::agent::planning

#endif // WUWE_AGENT_PLANNING_PLAN_REFLECTION_HPP
