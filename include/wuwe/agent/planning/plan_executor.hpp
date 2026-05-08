#ifndef WUWE_AGENT_PLANNING_PLAN_EXECUTOR_HPP
#define WUWE_AGENT_PLANNING_PLAN_EXECUTOR_HPP

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <wuwe/agent/planning/plan.hpp>
#include <wuwe/agent/tools/tool.hpp>

namespace wuwe::agent::planning {

struct plan_execution_context {
  const plan& current_plan;
  const std::map<std::string, nlohmann::json>& artifacts;
};

class plan_executor {
public:
  virtual ~plan_executor() = default;

  virtual plan_step_result execute(
    const plan_step& step,
    const plan_execution_context& context) = 0;
};

class function_plan_executor final : public plan_executor {
public:
  using callback = std::function<plan_step_result(const plan_step&, const plan_execution_context&)>;

  explicit function_plan_executor(callback execute) : execute_(std::move(execute)) {
  }

  plan_step_result execute(
    const plan_step& step,
    const plan_execution_context& context) override {
    return execute_(step, context);
  }

private:
  callback execute_;
};

class tool_plan_executor final : public plan_executor {
public:
  using tools_callback = std::function<std::vector<llm_tool>()>;
  using invoke_callback = std::function<llm_tool_result(const std::string&, const std::string&)>;

  tool_plan_executor(tools_callback tools, invoke_callback invoke)
      : tools_(std::move(tools)), invoke_(std::move(invoke)) {
  }

  template<typename ToolProvider>
  explicit tool_plan_executor(std::shared_ptr<ToolProvider> provider)
      : tools_([provider] { return provider->tools(); }),
        invoke_([provider](const std::string& name, const std::string& arguments_json) {
          return provider->invoke(name, arguments_json);
        }) {
  }

  plan_step_result execute(
    const plan_step& step,
    const plan_execution_context& context) override {
    (void)context;
    if (!step.assigned_tool || step.assigned_tool->empty()) {
      return plan_step_result::blocked("step has no assigned tool");
    }

    bool found = false;
    for (const auto& tool : tools_()) {
      if (tool.name == *step.assigned_tool) {
        found = true;
        break;
      }
    }
    if (!found) {
      return plan_step_result::blocked("tool not found: " + *step.assigned_tool);
    }

    const auto arguments = !step.input.empty()
                             ? step.input
                             : (step.input_json.is_object() ? step.input_json.dump() : std::string("{}"));
    const auto result = invoke_(*step.assigned_tool, arguments);
    if (result.error_code) {
      return plan_step_result {
        .status = plan_step_status::failed,
        .output = result.content,
        .error = result.error_code.message(),
      };
    }

    plan_step_result output = plan_step_result::completed(result.content);
    if (!result.content.empty()) {
      try {
        output.output_json = nlohmann::json::parse(result.content);
      }
      catch (...) {
      }
    }
    return output;
  }

private:
  tools_callback tools_;
  invoke_callback invoke_;
};

class agent_plan_executor final : public plan_executor {
public:
  using agent_callback =
    std::function<plan_step_result(const plan_step&, const plan_execution_context&)>;

  agent_plan_executor& add_agent(std::string name, agent_callback callback) {
    agents_[std::move(name)] = std::move(callback);
    return *this;
  }

  plan_step_result execute(
    const plan_step& step,
    const plan_execution_context& context) override {
    if (!step.assigned_agent || step.assigned_agent->empty()) {
      return plan_step_result::blocked("step has no assigned agent");
    }

    const auto found = agents_.find(*step.assigned_agent);
    if (found == agents_.end()) {
      return plan_step_result::blocked("agent not found: " + *step.assigned_agent);
    }

    return found->second(step, context);
  }

private:
  std::map<std::string, agent_callback> agents_;
};

class composite_plan_executor final : public plan_executor {
public:
  composite_plan_executor(
    std::shared_ptr<plan_executor> tool_executor,
    std::shared_ptr<plan_executor> agent_executor)
      : tool_executor_(std::move(tool_executor)), agent_executor_(std::move(agent_executor)) {
  }

  plan_step_result execute(
    const plan_step& step,
    const plan_execution_context& context) override {
    if (step.assigned_agent && agent_executor_) {
      return agent_executor_->execute(step, context);
    }
    if (step.assigned_tool && tool_executor_) {
      return tool_executor_->execute(step, context);
    }
    return plan_step_result::blocked("step has no assigned executor");
  }

private:
  std::shared_ptr<plan_executor> tool_executor_;
  std::shared_ptr<plan_executor> agent_executor_;
};

} // namespace wuwe::agent::planning

#endif // WUWE_AGENT_PLANNING_PLAN_EXECUTOR_HPP
