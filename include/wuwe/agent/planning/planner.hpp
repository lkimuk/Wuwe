#ifndef WUWE_AGENT_PLANNING_PLANNER_HPP
#define WUWE_AGENT_PLANNING_PLANNER_HPP

#include <algorithm>
#include <cstddef>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <wuwe/agent/llm/llm_client.h>
#include <wuwe/agent/planning/plan.hpp>

namespace wuwe::agent::planning {

class planner {
public:
  virtual ~planner() = default;

  virtual plan create_plan(const planning_request& request) = 0;

  virtual plan revise_plan(const plan& current, const planning_observation& observation) {
    (void)observation;
    return current;
  }
};

class static_planner final : public planner {
public:
  static_planner() = default;

  explicit static_planner(plan plan_template) : plan_template_(std::move(plan_template)) {
  }

  explicit static_planner(std::vector<plan_step> steps) {
    plan_template_.steps = std::move(steps);
  }

  plan create_plan(const planning_request& request) override {
    plan output = plan_template_;
    if (output.id.empty()) {
      output.id = "plan-1";
    }
    if (output.goal.empty()) {
      output.goal = request.goal;
    }
    if (output.steps.empty() && !request.goal.empty()) {
      output.steps.push_back({
        .id = "step-1",
        .title = request.goal,
        .description = request.input,
        .input = request.input,
      });
    }

    std::size_t index = 1;
    for (auto& step : output.steps) {
      if (step.id.empty()) {
        step.id = "step-" + std::to_string(index);
      }
      if (step.title.empty()) {
        step.title = step.id;
      }
      step.status = plan_step_status::pending;
      step.output.clear();
      step.error.clear();
      step.attempts = 0;
      ++index;
    }

    return output;
  }

  void set_plan(plan plan_template) {
    plan_template_ = std::move(plan_template);
  }

private:
  plan plan_template_;
};

struct llm_planner_options {
  std::string model;
  double temperature { 0.0 };
  bool repair_plan { true };
};

class llm_planner final : public planner {
public:
  explicit llm_planner(llm_client& client, llm_planner_options options = {})
      : client_(client), options_(std::move(options)) {
  }

  plan create_plan(const planning_request& request) override {
    llm_request llm_request;
    llm_request.model = options_.model;
    llm_request.temperature = options_.temperature;
    llm_request.response_format = "json_object";
    llm_request.messages.push_back({
      .role = "system",
      .content = request.system_prompt.empty() ? default_system_prompt() : request.system_prompt,
    });
    llm_request.messages.push_back({
      .role = "user",
      .content = create_prompt(request),
    });

    const auto response = client_.complete(llm_request);
    if (!response) {
      throw std::runtime_error("llm planner failed: " + response.error_code.message());
    }
    auto output = parse_plan(response.content, request);
    repair_if_enabled(output, request);
    ensure_valid_llm_plan(output, validation_options_from_request(request));
    return output;
  }

  plan revise_plan(const plan& current, const planning_observation& observation) override {
    llm_request llm_request;
    llm_request.model = options_.model;
    llm_request.temperature = options_.temperature;
    llm_request.response_format = "json_object";
    llm_request.messages.push_back({
      .role = "system",
      .content = default_system_prompt(),
    });
    llm_request.messages.push_back({
      .role = "user",
      .content = revise_prompt(current, observation),
    });

    const auto response = client_.complete(llm_request);
    if (!response) {
      throw std::runtime_error("llm replanner failed: " + response.error_code.message());
    }

    planning_request request {
      .goal = current.goal,
      .max_steps = current.steps.size(),
      .metadata = current.metadata,
    };
    auto output = parse_plan(response.content, request);
    repair_if_enabled(output, request);
    plan_validation_options options;
    options.max_steps = current.steps.size();
    ensure_valid_llm_plan(output, options);
    return output;
  }

private:
  static std::string default_system_prompt() {
    return "You are a production planning component. Return only a JSON object, without markdown. "
           "The object must have: id, goal, steps. Each step must be an executable action with "
           "id, title, description, input, depends_on, optional assigned_tool, optional "
           "assigned_agent, and metadata. Use assigned_tool only when it appears in the provided "
           "tool catalog. For tool steps, input must be a JSON object serialized as a string.";
  }

  static std::string create_prompt(const planning_request& request) {
    std::ostringstream out;
    out << "Create an executable plan.\nGoal: " << request.goal << "\n";
    if (!request.input.empty()) {
      out << "Input: " << request.input << "\n";
    }
    out << "Maximum steps: " << request.max_steps << "\n";
    if (!request.available_tools.empty()) {
      out << "Available tools:\n";
      for (const auto& tool : request.available_tools) {
        out << "- " << tool.name << ": " << tool.description << "\n";
        out << "  parameters_json_schema: " << tool.parameters_json_schema << "\n";
      }
    }
    if (!request.available_agents.empty()) {
      out << "Available agents:\n";
      for (const auto& agent : request.available_agents) {
        out << "- " << agent << "\n";
      }
    }
    out << "Return schema:\n"
        << "{\"id\":\"plan-id\",\"goal\":\"...\",\"steps\":[{\"id\":\"step-id\","
           "\"title\":\"...\",\"description\":\"...\",\"depends_on\":[],"
           "\"assigned_tool\":null,\"assigned_agent\":null,\"input\":\"{}\","
           "\"metadata\":{}}],\"metadata\":{}}\n";
    return out.str();
  }

  static std::string revise_prompt(const plan& current, const planning_observation& observation) {
    nlohmann::json current_json = plan_to_json(current);
    nlohmann::json observation_json {
      { "plan_id", observation.plan_id },
      { "step_id", observation.step_id },
      { "status", to_string(observation.status) },
      { "output", observation.output },
      { "error", observation.error },
      { "metadata", observation.metadata },
    };

    std::ostringstream out;
    out << "Revise this plan after the latest observation.\nCurrent plan:\n"
        << current_json.dump() << "\nObservation:\n" << observation_json.dump();
    return out.str();
  }

  static plan parse_plan(const std::string& content, const planning_request& request) {
    return plan_from_json_string(content, request.goal, request.max_steps, request.metadata);
  }

  void repair_if_enabled(plan& value, const planning_request& request) const {
    if (!options_.repair_plan) {
      return;
    }
    normalize_plan(value, {
      .clear_unknown_tools = false,
      .clear_unknown_agents = false,
      .available_tools = request.available_tools,
      .available_agents = request.available_agents,
    });
  }

  static void ensure_valid_llm_plan(const plan& value, plan_validation_options options) {
    const auto validation = validate_plan(value, std::move(options));
    if (!validation.valid()) {
      throw std::runtime_error("llm planner produced invalid plan: " + validation.message());
    }
  }

  llm_client& client_;
  llm_planner_options options_;
};

} // namespace wuwe::agent::planning

#endif // WUWE_AGENT_PLANNING_PLANNER_HPP
