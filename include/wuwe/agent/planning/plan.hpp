#ifndef WUWE_AGENT_PLANNING_PLAN_HPP
#define WUWE_AGENT_PLANNING_PLAN_HPP

#include <cstddef>
#include <algorithm>
#include <chrono>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <wuwe/agent/llm/llm_types.h>

namespace wuwe::agent::planning {

enum class plan_step_status {
  pending,
  running,
  completed,
  failed,
  skipped,
  blocked,
};

inline std::string to_string(plan_step_status status) {
  switch (status) {
  case plan_step_status::pending:
    return "pending";
  case plan_step_status::running:
    return "running";
  case plan_step_status::completed:
    return "completed";
  case plan_step_status::failed:
    return "failed";
  case plan_step_status::skipped:
    return "skipped";
  case plan_step_status::blocked:
    return "blocked";
  }
  return "unknown";
}

inline std::optional<plan_step_status> plan_step_status_from_string(const std::string& value) {
  if (value == "pending") {
    return plan_step_status::pending;
  }
  if (value == "running") {
    return plan_step_status::running;
  }
  if (value == "completed") {
    return plan_step_status::completed;
  }
  if (value == "failed") {
    return plan_step_status::failed;
  }
  if (value == "skipped") {
    return plan_step_status::skipped;
  }
  if (value == "blocked") {
    return plan_step_status::blocked;
  }
  return std::nullopt;
}

inline bool is_terminal(plan_step_status status) {
  return status == plan_step_status::completed || status == plan_step_status::failed ||
         status == plan_step_status::skipped || status == plan_step_status::blocked;
}

enum class plan_run_stop_reason {
  none,
  completed,
  failed,
  blocked,
  approval_required,
  no_ready_step,
  max_iterations,
  step_budget_exhausted,
  cancelled,
};

inline std::string to_string(plan_run_stop_reason reason) {
  switch (reason) {
  case plan_run_stop_reason::none:
    return "none";
  case plan_run_stop_reason::completed:
    return "completed";
  case plan_run_stop_reason::failed:
    return "failed";
  case plan_run_stop_reason::blocked:
    return "blocked";
  case plan_run_stop_reason::approval_required:
    return "approval_required";
  case plan_run_stop_reason::no_ready_step:
    return "no_ready_step";
  case plan_run_stop_reason::max_iterations:
    return "max_iterations";
  case plan_run_stop_reason::step_budget_exhausted:
    return "step_budget_exhausted";
  case plan_run_stop_reason::cancelled:
    return "cancelled";
  }
  return "unknown";
}

struct plan_step {
  std::string id;
  std::string title;
  std::string description;
  std::vector<std::string> depends_on;
  std::vector<std::string> input_from_steps;
  std::optional<std::string> assigned_tool;
  std::optional<std::string> assigned_agent;
  bool requires_approval { false };
  bool approved { false };
  plan_step_status status { plan_step_status::pending };
  std::string input;
  nlohmann::json input_json;
  std::string output;
  nlohmann::json output_json;
  std::string error;
  std::size_t attempts { 0 };
  std::vector<std::string> produced_artifacts;
  std::map<std::string, std::string> metadata;
};

struct plan {
  std::string id;
  std::string goal;
  std::vector<plan_step> steps;
  std::map<std::string, nlohmann::json> artifacts;
  std::map<std::string, std::string> metadata;
};

struct planning_request {
  std::string goal;
  std::string input;
  std::string system_prompt;
  std::size_t max_steps { 8 };
  std::vector<llm_tool> available_tools;
  std::vector<std::string> available_agents;
  std::map<std::string, std::string> metadata;
};

struct plan_policy {
  std::size_t max_steps { 8 };
  std::size_t max_iterations { 32 };
  std::size_t max_step_attempts { 1 };
  std::size_t max_steps_per_run { 0 };
  std::size_t max_parallel_steps { 1 };
  std::chrono::milliseconds step_timeout { 0 };
  std::chrono::milliseconds run_timeout { 0 };
  bool allow_replanning { false };
  bool continue_after_step_failure { false };
  bool reset_running_steps_on_resume { true };
};

struct plan_step_result {
  plan_step_status status { plan_step_status::completed };
  std::string output;
  nlohmann::json output_json;
  std::string error;
  std::map<std::string, nlohmann::json> artifacts;
  std::map<std::string, std::string> metadata;

  static plan_step_result completed(std::string output = {}) {
    return { .status = plan_step_status::completed, .output = std::move(output) };
  }

  static plan_step_result failed(std::string error) {
    return { .status = plan_step_status::failed, .error = std::move(error) };
  }

  static plan_step_result blocked(std::string reason) {
    return { .status = plan_step_status::blocked, .error = std::move(reason) };
  }
};

struct planning_observation {
  std::string plan_id;
  std::string step_id;
  plan_step_status status { plan_step_status::pending };
  std::string output;
  std::string error;
  std::map<std::string, std::string> metadata;
};

struct plan_run_result {
  plan value;
  bool completed { false };
  plan_run_stop_reason stop_reason { plan_run_stop_reason::none };
  std::size_t iterations { 0 };
  std::size_t steps_executed { 0 };
  std::size_t steps_completed { 0 };
  std::size_t steps_failed { 0 };
  std::size_t steps_blocked { 0 };
  std::chrono::milliseconds elapsed { 0 };
  std::string final_output;
  std::string error;
};

enum class plan_policy_decision {
  allow,
  deny,
  require_approval,
};

struct plan_policy_check {
  plan_policy_decision decision { plan_policy_decision::allow };
  std::string reason;
};

struct plan_approval_result {
  bool approved { false };
  std::string reason;
  std::map<std::string, std::string> metadata;
};

enum class plan_validation_severity {
  warning,
  error,
};

struct plan_validation_issue {
  plan_validation_severity severity { plan_validation_severity::error };
  std::string code;
  std::string message;
  std::string step_id;
};

struct plan_validation_options {
  std::size_t max_steps { 8 };
  bool require_goal { true };
  bool require_steps { true };
  bool require_unique_step_ids { true };
  bool require_known_dependencies { true };
  bool reject_dependency_cycles { true };
  bool require_known_tools { true };
  bool require_known_agents { false };
  bool require_tool_input_json { true };
  std::vector<llm_tool> available_tools;
  std::vector<std::string> available_agents;
};

struct plan_validation_result {
  std::vector<plan_validation_issue> issues;

  bool valid() const {
    return std::none_of(issues.begin(), issues.end(), [](const plan_validation_issue& issue) {
      return issue.severity == plan_validation_severity::error;
    });
  }

  std::string message() const {
    std::ostringstream out;
    bool first = true;
    for (const auto& issue : issues) {
      if (!first) {
        out << "; ";
      }
      first = false;
      out << issue.code << ": " << issue.message;
      if (!issue.step_id.empty()) {
        out << " [step=" << issue.step_id << "]";
      }
    }
    return out.str();
  }
};

struct plan_repair_options {
  bool fill_missing_ids { true };
  bool make_duplicate_ids_unique { true };
  bool fill_missing_titles { true };
  bool remove_unknown_dependencies { true };
  bool clear_unknown_tools { false };
  bool clear_unknown_agents { false };
  bool reset_running_steps { true };
  std::vector<llm_tool> available_tools;
  std::vector<std::string> available_agents;
};

namespace detail {

inline bool contains_tool(const std::vector<llm_tool>& tools, const std::string& name) {
  return std::any_of(tools.begin(), tools.end(), [&](const llm_tool& tool) {
    return tool.name == name;
  });
}

inline bool contains_agent(const std::vector<std::string>& agents, const std::string& name) {
  return std::find(agents.begin(), agents.end(), name) != agents.end();
}

inline bool looks_like_json_object(const std::string& value) {
  if (value.empty()) {
    return true;
  }
  try {
    const auto parsed = nlohmann::json::parse(value);
    return parsed.is_object();
  }
  catch (...) {
    return false;
  }
}

inline bool has_dependency_cycle_from(
  const std::string& id,
  const std::map<std::string, std::vector<std::string>>& graph,
  std::set<std::string>& visiting,
  std::set<std::string>& visited) {
  if (visited.contains(id)) {
    return false;
  }
  if (visiting.contains(id)) {
    return true;
  }

  visiting.insert(id);
  const auto found = graph.find(id);
  if (found != graph.end()) {
    for (const auto& dependency : found->second) {
      if (has_dependency_cycle_from(dependency, graph, visiting, visited)) {
        return true;
      }
    }
  }
  visiting.erase(id);
  visited.insert(id);
  return false;
}

} // namespace detail

class plan_validator {
public:
  explicit plan_validator(plan_validation_options options = {}) : options_(std::move(options)) {
  }

  plan_validation_result validate(const plan& value) const {
    plan_validation_result result;

    auto add_error = [&](std::string code, std::string message, std::string step_id = {}) {
      result.issues.push_back({
        .severity = plan_validation_severity::error,
        .code = std::move(code),
        .message = std::move(message),
        .step_id = std::move(step_id),
      });
    };

    if (options_.require_goal && value.goal.empty()) {
      add_error("missing_goal", "plan goal must not be empty");
    }
    if (options_.require_steps && value.steps.empty()) {
      add_error("missing_steps", "plan must contain at least one step");
    }
    if (options_.max_steps != 0 && value.steps.size() > options_.max_steps) {
      add_error("too_many_steps", "plan exceeds maximum step count");
    }

    std::set<std::string> ids;
    std::map<std::string, std::vector<std::string>> graph;

    for (const auto& step : value.steps) {
      if (step.id.empty()) {
        add_error("missing_step_id", "step id must not be empty");
        continue;
      }

      if (options_.require_unique_step_ids && !ids.insert(step.id).second) {
        add_error("duplicate_step_id", "step id must be unique", step.id);
      }

      if (step.title.empty() && step.description.empty()) {
        add_error("empty_step", "step must have a title or description", step.id);
      }
      if (step.approved && !step.requires_approval) {
        add_error("approval_without_gate", "step approval is set but approval is not required", step.id);
      }

      graph[step.id] = step.depends_on;

      for (const auto& dependency : step.depends_on) {
        if (dependency == step.id) {
          add_error("self_dependency", "step cannot depend on itself", step.id);
        }
        if (options_.require_known_dependencies &&
            std::none_of(value.steps.begin(), value.steps.end(), [&](const plan_step& candidate) {
              return candidate.id == dependency;
            })) {
          add_error("unknown_dependency", "step depends on unknown step: " + dependency, step.id);
        }
      }

      if (step.assigned_tool && options_.require_known_tools && !options_.available_tools.empty() &&
          !detail::contains_tool(options_.available_tools, *step.assigned_tool)) {
        add_error("unknown_tool", "step assigned unknown tool: " + *step.assigned_tool, step.id);
      }

      if (step.assigned_agent && options_.require_known_agents && !options_.available_agents.empty() &&
          !detail::contains_agent(options_.available_agents, *step.assigned_agent)) {
        add_error("unknown_agent", "step assigned unknown agent: " + *step.assigned_agent, step.id);
      }

      if (step.assigned_tool && options_.require_tool_input_json &&
          !detail::looks_like_json_object(step.input)) {
        add_error("invalid_tool_input", "tool step input must be a JSON object string", step.id);
      }
    }

    if (options_.reject_dependency_cycles) {
      std::set<std::string> visiting;
      std::set<std::string> visited;
      for (const auto& [id, _] : graph) {
        if (detail::has_dependency_cycle_from(id, graph, visiting, visited)) {
          add_error("dependency_cycle", "plan contains a dependency cycle", id);
          break;
        }
      }
    }

    return result;
  }

private:
  plan_validation_options options_;
};

class plan_normalizer {
public:
  explicit plan_normalizer(plan_repair_options options = {}) : options_(std::move(options)) {
  }

  void normalize(plan& value) const {
    std::map<std::string, std::size_t> seen;
    std::set<std::string> known_ids;

    for (std::size_t index = 0; index < value.steps.size(); ++index) {
      auto& step = value.steps[index];
      if (options_.fill_missing_ids && step.id.empty()) {
        step.id = "step-" + std::to_string(index + 1);
      }
      if (options_.make_duplicate_ids_unique && !step.id.empty()) {
        auto& count = seen[step.id];
        ++count;
        if (count > 1) {
          step.id += "-" + std::to_string(count);
        }
      }
      if (options_.fill_missing_titles && step.title.empty()) {
        step.title = step.description.empty() ? step.id : step.description;
      }
      if (options_.reset_running_steps && step.status == plan_step_status::running) {
        step.status = plan_step_status::pending;
      }
      known_ids.insert(step.id);
    }

    for (auto& step : value.steps) {
      if (options_.remove_unknown_dependencies) {
        std::erase_if(step.depends_on, [&](const std::string& dependency) {
          return !known_ids.contains(dependency) || dependency == step.id;
        });
        std::erase_if(step.input_from_steps, [&](const std::string& source) {
          return !known_ids.contains(source) || source == step.id;
        });
      }
      if (options_.clear_unknown_tools && step.assigned_tool && !options_.available_tools.empty() &&
          !detail::contains_tool(options_.available_tools, *step.assigned_tool)) {
        step.assigned_tool.reset();
      }
      if (options_.clear_unknown_agents && step.assigned_agent && !options_.available_agents.empty() &&
          !detail::contains_agent(options_.available_agents, *step.assigned_agent)) {
        step.assigned_agent.reset();
      }
    }
  }

private:
  plan_repair_options options_;
};

inline plan_validation_result validate_plan(
  const plan& value,
  plan_validation_options options = {}) {
  return plan_validator(std::move(options)).validate(value);
}

inline void normalize_plan(plan& value, plan_repair_options options = {}) {
  plan_normalizer(std::move(options)).normalize(value);
}

inline plan repaired_plan(plan value, plan_repair_options options = {}) {
  normalize_plan(value, std::move(options));
  return value;
}

inline plan_validation_options validation_options_from_request(const planning_request& request) {
  return {
    .max_steps = request.max_steps,
    .available_tools = request.available_tools,
    .available_agents = request.available_agents,
  };
}

class plan_codec {
public:
  static std::string extract_json_object(std::string content) {
    const auto first = content.find('{');
    const auto last = content.rfind('}');
    if (first == std::string::npos || last == std::string::npos || last < first) {
      return content;
    }
    return content.substr(first, last - first + 1);
  }

  static nlohmann::json step_to_json(const plan_step& step) {
    nlohmann::json item {
      { "id", step.id },
      { "title", step.title },
      { "description", step.description },
      { "depends_on", step.depends_on },
      { "input_from_steps", step.input_from_steps },
      { "requires_approval", step.requires_approval },
      { "approved", step.approved },
      { "status", to_string(step.status) },
      { "input", step.input },
      { "input_json", step.input_json.is_discarded() ? nlohmann::json(nullptr) : step.input_json },
      { "output", step.output },
      { "output_json", step.output_json.is_discarded() ? nlohmann::json(nullptr) : step.output_json },
      { "error", step.error },
      { "attempts", step.attempts },
      { "produced_artifacts", step.produced_artifacts },
      { "metadata", step.metadata },
    };
    item["assigned_tool"] =
      step.assigned_tool ? nlohmann::json(*step.assigned_tool) : nlohmann::json(nullptr);
    item["assigned_agent"] =
      step.assigned_agent ? nlohmann::json(*step.assigned_agent) : nlohmann::json(nullptr);
    return item;
  }

  static nlohmann::json to_json(const plan& value) {
    nlohmann::json steps = nlohmann::json::array();
    for (const auto& step : value.steps) {
      steps.push_back(step_to_json(step));
    }
    return {
      { "id", value.id },
      { "goal", value.goal },
      { "steps", std::move(steps) },
      { "artifacts", value.artifacts },
      { "metadata", value.metadata },
    };
  }

  static plan_step step_from_json(const nlohmann::json& item, std::size_t index = 0) {
    plan_step step;
    step.id = item.value("id", "step-" + std::to_string(index + 1));
    step.title = item.value("title", step.id);
    step.description = item.value("description", std::string {});
    step.depends_on = item.value("depends_on", std::vector<std::string> {});
    step.input_from_steps = item.value("input_from_steps", std::vector<std::string> {});
    step.requires_approval = item.value("requires_approval", false);
    step.approved = item.value("approved", false);
    if (item.contains("assigned_tool") && !item.at("assigned_tool").is_null()) {
      step.assigned_tool = item.at("assigned_tool").get<std::string>();
    }
    if (item.contains("assigned_agent") && !item.at("assigned_agent").is_null()) {
      step.assigned_agent = item.at("assigned_agent").get<std::string>();
    }
    if (item.contains("status") && item.at("status").is_string()) {
      step.status =
        plan_step_status_from_string(item.at("status").get<std::string>()).value_or(plan_step_status::pending);
    }
    step.input = item.value("input", std::string {});
    if (item.contains("input_json") && !item.at("input_json").is_null()) {
      step.input_json = item.at("input_json");
    }
    step.output = item.value("output", std::string {});
    if (item.contains("output_json") && !item.at("output_json").is_null()) {
      step.output_json = item.at("output_json");
    }
    step.error = item.value("error", std::string {});
    step.attempts = item.value("attempts", std::size_t {});
    step.produced_artifacts = item.value("produced_artifacts", std::vector<std::string> {});
    step.metadata = item.value("metadata", std::map<std::string, std::string> {});
    return step;
  }

  static plan from_json(
    const nlohmann::json& json,
    std::string fallback_goal = {},
    std::size_t max_steps = 0,
    std::map<std::string, std::string> fallback_metadata = {}) {
    plan output;
    output.id = json.value("id", std::string("plan-1"));
    output.goal = json.value("goal", std::move(fallback_goal));
    if (json.contains("artifacts") && json.at("artifacts").is_object()) {
      output.artifacts = json.at("artifacts").get<std::map<std::string, nlohmann::json>>();
    }
    output.metadata = json.value("metadata", std::move(fallback_metadata));

    const auto steps = json.at("steps");
    if (!steps.is_array()) {
      throw std::runtime_error("plan JSON steps must be an array");
    }

    const auto limit = max_steps == 0 ? steps.size() : (std::min)(max_steps, steps.size());
    for (std::size_t i = 0; i < limit; ++i) {
      output.steps.push_back(step_from_json(steps.at(i), i));
    }
    return output;
  }

  static plan from_json_string(
    const std::string& content,
    std::string fallback_goal = {},
    std::size_t max_steps = 0,
    std::map<std::string, std::string> fallback_metadata = {}) {
    return from_json(nlohmann::json::parse(extract_json_object(content)),
      std::move(fallback_goal),
      max_steps,
      std::move(fallback_metadata));
  }
};

inline std::string extract_json_object(std::string content) {
  return plan_codec::extract_json_object(std::move(content));
}

inline nlohmann::json plan_step_to_json(const plan_step& step) {
  return plan_codec::step_to_json(step);
}

inline nlohmann::json plan_to_json(const plan& value) {
  return plan_codec::to_json(value);
}

inline plan_step plan_step_from_json(const nlohmann::json& item, std::size_t index = 0) {
  return plan_codec::step_from_json(item, index);
}

inline plan plan_from_json(
  const nlohmann::json& json,
  std::string fallback_goal = {},
  std::size_t max_steps = 0,
  std::map<std::string, std::string> fallback_metadata = {}) {
  return plan_codec::from_json(json, std::move(fallback_goal), max_steps, std::move(fallback_metadata));
}

inline plan plan_from_json_string(
  const std::string& content,
  std::string fallback_goal = {},
  std::size_t max_steps = 0,
  std::map<std::string, std::string> fallback_metadata = {}) {
  return plan_codec::from_json_string(content, std::move(fallback_goal), max_steps, std::move(fallback_metadata));
}

} // namespace wuwe::agent::planning

#endif // WUWE_AGENT_PLANNING_PLAN_HPP
