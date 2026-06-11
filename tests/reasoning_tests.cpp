#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <wuwe/agent/reasoning/reasoning.hpp>
#include <wuwe/common/print.h>

namespace {

using namespace wuwe;
namespace planning = wuwe::agent::planning;
namespace reasoning = wuwe::agent::reasoning;
namespace reflection = wuwe::agent::reflection;

void require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

class scripted_llm_client final : public llm_client {
public:
  explicit scripted_llm_client(std::vector<llm_response> responses)
      : responses_(std::move(responses)) {
  }

  llm_response complete(const llm_request& request) override {
    requests.push_back(request);
    if (responses_.empty()) {
      return { .content = "default" };
    }
    auto response = responses_.front();
    responses_.erase(responses_.begin());
    return response;
  }

  std::vector<llm_request> requests;

private:
  std::vector<llm_response> responses_;
};

struct echo_tool {
  static constexpr std::string_view description = "Echo text back to the caller.";

  std::string text;

  std::string invoke() const {
    return text;
  }
};

class scripted_reflector final : public reflection::reflector {
public:
  explicit scripted_reflector(std::vector<reflection::reflection_result> results)
      : results_(std::move(results)) {
  }

  reflection::reflection_result reflect(const reflection::reflection_request& request) override {
    requests.push_back(request);
    if (results_.empty()) {
      return reflection::reflection_result::pass();
    }
    auto result = results_.front();
    results_.erase(results_.begin());
    return result;
  }

  std::vector<reflection::reflection_request> requests;

private:
  std::vector<reflection::reflection_result> results_;
};

void simple_mode_runs_model_and_emits_events() {
  scripted_llm_client client({ { .content = "simple answer" } });
  std::vector<reasoning::reasoning_event_type> events;

  reasoning::reasoning_runner runner(client, [&](const reasoning::reasoning_event& event) {
    events.push_back(event.type);
  });

  auto result = runner.run({
    .input = "answer simply",
    .policy = {
      .mode = reasoning::reasoning_mode::simple,
    },
  });

  require(result.completed, "simple reasoning completes");
  require(result.content == "simple answer", "simple reasoning returns model content");
  require(client.requests.size() == 1, "simple reasoning calls model once");
  require(result.usage.model_calls == 1, "simple reasoning records model usage");
  require(!result.trace.empty(), "simple reasoning records a trace");
  require(result.trace.front().type == reasoning::reasoning_event_type::started,
    "simple reasoning trace starts with started");
  require(result.trace.back().type == reasoning::reasoning_event_type::completed,
    "simple reasoning trace ends with completed");
  require(events.front() == reasoning::reasoning_event_type::started,
    "simple reasoning emits started");
  require(events.back() == reasoning::reasoning_event_type::completed,
    "simple reasoning emits completed");
}

void react_mode_uses_tool_provider() {
  scripted_llm_client client({
    {
      .tool_calls = {
        {
          .id = "call-1",
          .name = "echo_tool",
          .arguments_json = R"({"text":"from tool"})",
        },
      },
    },
    { .content = "final with tool" },
  });

  auto provider = std::make_shared<tool_provider<echo_tool>>();
  std::vector<reasoning::reasoning_event_type> events;
  auto runner = reasoning::reasoning_runner::with_tools(client, provider, {
    .observer = [&](const reasoning::reasoning_event& event) {
      events.push_back(event.type);
    },
  });

  auto result = runner.run({
    .input = "use a tool",
    .policy = {
      .mode = reasoning::reasoning_mode::react,
      .budget = {
        .max_tool_rounds = 2,
      },
    },
  });

  require(result.completed, "react reasoning completes");
  require(result.content == "final with tool", "react reasoning returns final content");
  require(client.requests.size() == 2, "react reasoning performs follow-up model call");
  require(result.usage.model_calls == 2, "react reasoning records each model call");
  require(result.usage.tool_calls == 1, "react reasoning records tool usage");
  require(std::find(events.begin(), events.end(), reasoning::reasoning_event_type::tool_started) !=
      events.end(),
    "react reasoning emits tool start");
  require(std::find(events.begin(), events.end(), reasoning::reasoning_event_type::tool_completed) !=
      events.end(),
    "react reasoning emits tool result");
}

void simple_mode_with_tool_provider_does_not_execute_tools() {
  scripted_llm_client client({
    {
      .tool_calls = {
        {
          .id = "call-1",
          .name = "echo_tool",
          .arguments_json = R"({"text":"from tool"})",
        },
      },
    },
    { .content = "should not be requested" },
  });

  auto provider = std::make_shared<tool_provider<echo_tool>>();
  auto runner = reasoning::reasoning_runner::with_tools(client, provider);

  auto result = runner.run({
    .input = "answer simply",
    .policy = {
      .mode = reasoning::reasoning_mode::simple,
    },
  });

  require(result.completed, "simple reasoning still completes with tool-shaped response");
  require(client.requests.size() == 1, "simple reasoning does not follow up with tools");
  require(result.usage.model_calls == 1, "simple reasoning with provider records one model call");
  require(result.usage.tool_calls == 0, "simple reasoning with provider records no tool usage");
}

void reflect_and_retry_retries_until_reflection_passes() {
  scripted_llm_client client({
    { .content = "bad answer" },
    { .content = "good answer" },
  });

  auto reflector = std::make_shared<scripted_reflector>(std::vector<reflection::reflection_result> {
    {
      .passed = false,
      .score = 0.5,
      .recommended_action = reflection::reflection_action::retry,
      .issues = {
        {
          .severity = reflection::reflection_severity::warning,
          .code = "thin",
          .message = "answer is thin",
          .suggestion = "add detail",
        },
      },
    },
    reflection::reflection_result::pass(),
  });
  auto reflection_runner = std::make_shared<reflection::reflection_runner>(
    reflection::reflection_runner_options {
      .reflector = reflector,
    });

  reasoning::reasoning_runner runner({
    .client = &client,
    .reflection = reflection_runner,
  });

  auto result = runner.run({
    .input = "make it good",
    .policy = {
      .mode = reasoning::reasoning_mode::reflect_and_retry,
      .budget = {
        .max_reflection_attempts = 2,
      },
    },
  });

  require(result.completed, "reflect-and-retry completes after passing reflection");
  require(result.content == "good answer", "reflect-and-retry returns passing output");
  require(result.reflections.size() == 2, "reflect-and-retry records reflection attempts");
  require(client.requests.size() == 2, "reflect-and-retry calls model twice");
  require(result.usage.model_calls == 2, "reflect-and-retry records model usage");
  require(result.usage.reflection_calls == 2, "reflect-and-retry records reflection usage");
  require(std::find_if(result.trace.begin(),
            result.trace.end(),
            [](const reasoning::reasoning_trace_record& record) {
              return record.type == reasoning::reasoning_event_type::model_started &&
                     record.mode == reasoning::reasoning_mode::reflect_and_retry;
            }) != result.trace.end(),
    "reflect-and-retry labels model events with the outer reasoning mode");
  require(client.requests[1].messages.back().content.find("Reflection feedback") !=
      std::string::npos,
    "reflect-and-retry feeds critique into retry prompt");
}

void model_budget_stops_retry_before_second_model_call() {
  scripted_llm_client client({
    { .content = "bad answer" },
    { .content = "should not be requested" },
  });

  auto reflector = std::make_shared<scripted_reflector>(std::vector<reflection::reflection_result> {
    {
      .passed = false,
      .score = 0.5,
      .recommended_action = reflection::reflection_action::retry,
      .issues = {
        {
          .severity = reflection::reflection_severity::warning,
          .code = "thin",
          .message = "answer is thin",
        },
      },
    },
  });
  auto reflection_runner = std::make_shared<reflection::reflection_runner>(
    reflection::reflection_runner_options {
      .reflector = reflector,
    });

  reasoning::reasoning_runner runner({
    .client = &client,
    .reflection = reflection_runner,
  });

  auto result = runner.run({
    .input = "make it good",
    .policy = {
      .mode = reasoning::reasoning_mode::reflect_and_retry,
      .budget = {
        .max_model_calls = 1,
        .max_reflection_attempts = 2,
      },
    },
  });

  require(!result.completed, "model budget stops reflect-and-retry");
  require(result.error.find("model call budget") != std::string::npos,
    "model budget reports a clear error");
  require(client.requests.size() == 1, "model budget prevents second provider call");
  require(result.usage.model_calls == 1, "model budget records completed model calls");
  require(result.usage.reflection_calls == 1, "model budget records completed reflection calls");
  require(result.trace.back().type == reasoning::reasoning_event_type::failed,
    "model budget trace ends with failure");
}

void tool_budget_stops_tool_before_invocation() {
  scripted_llm_client client({
    {
      .tool_calls = {
        {
          .id = "call-1",
          .name = "echo_tool",
          .arguments_json = R"({"text":"from tool"})",
        },
      },
    },
  });

  auto provider = std::make_shared<tool_provider<echo_tool>>();
  auto runner = reasoning::reasoning_runner::with_tools(client, provider);

  auto result = runner.run({
    .input = "use a tool",
    .policy = {
      .mode = reasoning::reasoning_mode::react,
      .budget = {
        .max_tool_calls = 0,
        .max_tool_rounds = 2,
      },
    },
  });

  require(!result.completed, "tool budget stops react reasoning");
  require(result.error.find("tool call budget") != std::string::npos,
    "tool budget reports a clear error");
  require(result.usage.tool_calls == 0, "tool budget prevents tool invocation");
}

void plan_execute_mode_delegates_to_planning() {
  auto planner = std::make_shared<planning::static_planner>(std::vector<planning::plan_step> {
    {
      .id = "first",
      .title = "First",
    },
    {
      .id = "second",
      .title = "Second",
      .depends_on = { "first" },
    },
  });

  std::vector<std::string> executed;
  auto executor = std::make_shared<planning::function_plan_executor>(
    [&](const planning::plan_step& step, const planning::plan_execution_context&) {
      executed.push_back(step.id);
      return planning::plan_step_result::completed("done-" + step.id);
    });

  reasoning::reasoning_runner runner({
    .planner = planner,
    .executor = executor,
  });

  auto result = runner.run({
    .input = "run the plan",
    .policy = {
      .mode = reasoning::reasoning_mode::plan_execute,
      .budget = {
        .max_steps = 4,
      },
    },
  });

  require(result.completed, "plan reasoning completes");
  require(result.content == "done-second", "plan reasoning returns plan final output");
  require(result.plan.has_value(), "plan reasoning stores plan result");
  require(executed.size() == 2, "plan reasoning executes both steps");
  require(result.usage.plan_steps == 2, "plan reasoning records plan step usage");
  require(executed[0] == "first" && executed[1] == "second",
    "plan reasoning respects dependencies");
}

void run(const char* name, void (*test)()) {
  test();
  println("[PASS] {}", name);
}

} // namespace

int main() {
  try {
    run("simple mode runs model and emits events", simple_mode_runs_model_and_emits_events);
    run("react mode uses tool provider", react_mode_uses_tool_provider);
    run("simple mode with tool provider does not execute tools",
      simple_mode_with_tool_provider_does_not_execute_tools);
    run("reflect and retry retries until reflection passes",
      reflect_and_retry_retries_until_reflection_passes);
    run("model budget stops retry before second model call",
      model_budget_stops_retry_before_second_model_call);
    run("tool budget stops tool before invocation", tool_budget_stops_tool_before_invocation);
    run("plan execute mode delegates to planning", plan_execute_mode_delegates_to_planning);
  }
  catch (const std::exception& ex) {
    println("[FAIL] {}", ex.what());
    return 1;
  }

  return 0;
}
