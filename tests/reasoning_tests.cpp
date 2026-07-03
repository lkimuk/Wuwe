#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
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

class streaming_tool_call_llm_client final : public llm_client {
public:
  bool supports_streaming() const noexcept override {
    return true;
  }

  llm_response complete(const llm_request& request) override {
    requests.push_back(request);
    return {
      .content = "draft",
      .tool_calls = {
        {
          .id = "call-1",
          .name = "echo_tool",
          .arguments_json = R"({"text":"from stream"})",
        },
      },
    };
  }

  llm_response complete_stream(
    const llm_request& request,
    const llm_stream_callbacks& callbacks,
    std::stop_token stop_token = {}) override {
    if (stop_token.stop_requested()) {
      return { .error_code = agent::make_error_code(agent::llm_error_code::cancelled) };
    }
    requests.push_back(request);
    llm_response response {
      .content = "draft",
      .reasoning_summary = "checking streamed tool",
      .tool_calls = {
        {
          .id = "call-1",
          .name = "echo_tool",
          .arguments_json = R"({"text":"from stream"})",
        },
      },
    };
    if (callbacks.on_event) {
      callbacks.on_event({
        .type = llm_stream_event_type::reasoning_delta,
        .reasoning_delta = "checking streamed tool",
      });
      callbacks.on_event({
        .type = llm_stream_event_type::content_delta,
        .content_delta = "draft",
      });
      callbacks.on_event({
        .type = llm_stream_event_type::tool_call_delta,
        .tool_call_delta = llm_tool_call_delta {
          .index = 0,
          .id = "call-1",
          .name_delta = "echo_",
          .arguments_delta = R"({"text")",
        },
      });
      callbacks.on_event({
        .type = llm_stream_event_type::tool_call_delta,
        .tool_call_delta = llm_tool_call_delta {
          .index = 0,
          .name_delta = "tool",
          .arguments_delta = R"(: "from stream"})",
        },
      });
      callbacks.on_event({
        .type = llm_stream_event_type::tool_call_done,
        .tool_call = response.tool_calls.front(),
      });
      callbacks.on_event({
        .type = llm_stream_event_type::reasoning_done,
        .reasoning_summary = response.reasoning_summary,
        .response = response,
      });
      callbacks.on_event({
        .type = llm_stream_event_type::done,
        .response = response,
      });
    }
    return response;
  }

  std::vector<llm_request> requests;
};

class cancellable_llm_client final : public llm_client {
public:
  llm_response complete(const llm_request& request) override {
    requests.push_back(request);
    return { .content = "late answer" };
  }

  llm_response complete(const llm_request& request, std::stop_token stop_token) override {
    requests.push_back(request);
    for (int index = 0; index < 100; ++index) {
      if (stop_token.stop_requested()) {
        return { .error_code = agent::make_error_code(agent::llm_error_code::cancelled) };
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return { .content = "late answer" };
  }

  std::vector<llm_request> requests;
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
    .language = {
      .response_language = "zh-CN",
      .reasoning_language = "zh-CN",
    },
    .policy = {
      .mode = reasoning::reasoning_mode::simple,
    },
  });

  require(result.completed, "simple reasoning completes");
  require(result.content == "simple answer", "simple reasoning returns model content");
  require(client.requests.size() == 1, "simple reasoning calls model once");
  require(client.requests.front().language.response_language == "zh-CN",
    "reasoning runner should propagate response language to LLM requests");
  require(client.requests.front().language.reasoning_language == "zh-CN",
    "reasoning runner should propagate reasoning language to LLM requests");
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

void async_run_reports_deltas_and_done() {
  scripted_llm_client client({ { .content = "async answer" } });
  reasoning::reasoning_runner runner(client);
  std::string streamed;
  std::atomic<bool> done { false };

  auto run = runner.run_async({
      .input = "answer asynchronously",
      .policy = {
        .mode = reasoning::reasoning_mode::simple,
      },
    },
    {
      .callbacks = {
        .on_delta = [&](std::string_view delta) {
          streamed += delta;
        },
        .on_done = [&](const reasoning::reasoning_result& result) {
          done = result.completed;
        },
      },
    });

  auto result = run.get();
  require(result.completed, "async reasoning completes");
  require(result.content == "async answer", "async reasoning returns content");
  require(streamed == "async answer", "async reasoning reports content deltas");
  require(done.load(), "async reasoning invokes done callback");
}

void async_run_can_be_cancelled() {
  cancellable_llm_client client;
  reasoning::reasoning_runner runner(client);
  std::atomic<bool> cancelled { false };

  auto run = runner.run_async({
      .input = "cancel this",
      .policy = {
        .mode = reasoning::reasoning_mode::simple,
      },
    },
    {
      .callbacks = {
        .on_cancelled = [&](const reasoning::reasoning_result& result) {
          cancelled = result.reasoning_error == reasoning::reasoning_error_code::cancelled;
        },
      },
    });

  run.request_stop();
  auto result = run.get();
  require(!result.completed, "cancelled async reasoning does not complete");
  require(result.reasoning_error == reasoning::reasoning_error_code::cancelled,
    "cancelled async reasoning reports reasoning cancellation");
  require(cancelled.load(), "cancelled async reasoning invokes cancellation callback");
}

void llm_errors_are_mapped_to_reasoning_errors() {
  scripted_llm_client client({
    {
      .error_code = agent::make_error_code(agent::llm_error_code::missing_api_key),
    },
  });
  reasoning::reasoning_runner runner(client);
  std::atomic<bool> saw_error { false };

  auto result = runner.run({
      .input = "needs a key",
      .policy = {
        .mode = reasoning::reasoning_mode::simple,
      },
    },
    {
      .callbacks = {
        .on_error = [&](const reasoning::reasoning_error& error) {
          saw_error = error.code == reasoning::reasoning_error_code::missing_api_key &&
                      error.underlying_error ==
                        agent::make_error_code(agent::llm_error_code::missing_api_key);
        },
      },
    });

  require(!result.completed, "llm error fails reasoning");
  require(result.reasoning_error == reasoning::reasoning_error_code::missing_api_key,
    "reasoning preserves missing api key as stable error");
  require(result.underlying_error ==
      agent::make_error_code(agent::llm_error_code::missing_api_key),
    "reasoning preserves underlying llm error");
  require(saw_error.load(), "reasoning invokes error callback with mapped code");
}

void reflect_mode_preserves_model_error_details() {
  scripted_llm_client client({
    {
      .error_code = agent::make_error_code(agent::llm_error_code::authentication_failed),
    },
  });
  auto reflection_runner = std::make_shared<reflection::reflection_runner>(
    reflection::reflection_runner_options {
      .reflector = std::make_shared<scripted_reflector>(
        std::vector<reflection::reflection_result> {}),
    });
  reasoning::reasoning_runner runner({
    .client = &client,
    .reflection = reflection_runner,
  });

  auto result = runner.run({
    .input = "fail before reflection",
    .policy = {
      .mode = reasoning::reasoning_mode::reflect_and_retry,
    },
  });

  require(!result.completed, "reflect mode stops when the model fails");
  require(result.reasoning_error == reasoning::reasoning_error_code::authentication_failed,
    "reflect mode preserves model reasoning error");
  require(result.underlying_error ==
      agent::make_error_code(agent::llm_error_code::authentication_failed),
    "reflect mode preserves underlying model error");
}

void policy_selector_and_trace_json_are_stable() {
  auto simple = reasoning::select_policy({
    .input = "What is this?",
  });
  require(simple.mode == reasoning::reasoning_mode::simple,
    "policy selector chooses simple for plain answers");

  auto planned = reasoning::select_policy({
    .input = "Create a multi-step workflow",
  });
  require(planned.mode == reasoning::reasoning_mode::plan_execute,
    "policy selector chooses planning for multi-step workflow");

  reasoning::reasoning_trace_record record {
    .sequence = 1,
    .type = reasoning::reasoning_event_type::completed,
    .mode = reasoning::reasoning_mode::simple,
    .message = "done",
  };
  auto json = reasoning::reasoning_trace_to_json({ record });
  require(json.is_array() && json.size() == 1, "trace json exports an array");
  require(json.at(0).at("type") == "completed", "trace json exports event type");
  require(json.at(0).at("mode") == "simple", "trace json exports reasoning mode");
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

void react_mode_maps_agent_stream_events_to_reasoning_events() {
  streaming_tool_call_llm_client client;
  auto provider = std::make_shared<tool_provider<echo_tool>>();

  std::vector<reasoning::reasoning_event_type> events;
  std::string streamed_reasoning;
  auto runner = reasoning::reasoning_runner::with_tools(client, provider, {
    .observer = [&](const reasoning::reasoning_event& event) {
      events.push_back(event.type);
    },
  });

  auto result = runner.run({
    .input = "use a streamed tool",
    .policy = {
      .mode = reasoning::reasoning_mode::react,
      .budget = {
        .max_tool_rounds = 0,
      },
      .enable_streaming = true,
    },
  },
  {
    .callbacks = {
      .on_reasoning_delta = [&](std::string_view delta) {
        streamed_reasoning += delta;
      },
    },
  });

  require(!result.completed,
    "streamed tool call with zero tool rounds should stop before tool execution");
  require(result.reasoning_summary == "checking streamed tool",
    "reasoning should preserve final provider reasoning summary");
  require(streamed_reasoning == "checking streamed tool",
    "reasoning callbacks should receive provider reasoning deltas");
  const auto has = [&](reasoning::reasoning_event_type type) {
    return std::find(events.begin(), events.end(), type) != events.end();
  };
  require(has(reasoning::reasoning_event_type::model_first_event),
    "reasoning should map agent model_first_event");
  require(has(reasoning::reasoning_event_type::content_delta),
    "reasoning should preserve streamed content deltas");
  require(has(reasoning::reasoning_event_type::reasoning_delta),
    "reasoning should preserve streamed reasoning deltas");
  require(has(reasoning::reasoning_event_type::reasoning_completed),
    "reasoning should preserve reasoning completion");
  require(has(reasoning::reasoning_event_type::tool_call_building),
    "reasoning should map streamed tool call deltas");
  require(has(reasoning::reasoning_event_type::tool_call_ready),
    "reasoning should map completed streamed tool calls");
  require(has(reasoning::reasoning_event_type::model_completed),
    "reasoning should map model completion");
}

void default_agentic_runner_wires_standard_capabilities() {
  scripted_llm_client client({ { .content = "default answer" } });
  auto provider = std::make_shared<tool_provider<echo_tool>>();
  auto runner = reasoning::make_default_agentic_runner(client, provider);

  auto simple = runner.run({
    .input = "answer simply",
    .policy = reasoning::select_policy(reasoning::reasoning_task_profile::simple_answer),
  });
  require(simple.completed, "default agentic runner handles simple mode");
  require(simple.content == "default answer", "default agentic runner returns simple content");

  auto reflected = runner.run({
    .input = "reflect",
    .policy = reasoning::select_policy(reasoning::reasoning_task_profile::high_confidence_answer),
  });
  require(reflected.completed, "default agentic runner includes reflection support");

  auto planned = runner.run({
    .input = "plan this",
    .policy = reasoning::select_policy(reasoning::reasoning_task_profile::plan_required),
  });
  require(planned.completed, "default agentic runner includes planning support");
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

void tool_round_budget_maps_to_stable_reasoning_error() {
  scripted_llm_client client({
    {
      .content = "need tool",
      .tool_calls = {
        {
          .id = "call-1",
          .name = "echo_tool",
          .arguments_json = R"({"text":"from tool"})",
        },
      },
    },
    {
      .content = "need tool again",
      .tool_calls = {
        {
          .id = "call-2",
          .name = "echo_tool",
          .arguments_json = R"({"text":"again"})",
        },
      },
    },
  });

  auto provider = std::make_shared<tool_provider<echo_tool>>();
  auto runner = reasoning::reasoning_runner::with_tools(client, provider);
  std::atomic<bool> saw_error { false };

  auto result = runner.run({
      .input = "use tools until exhausted",
      .policy = {
        .mode = reasoning::reasoning_mode::react,
        .budget = {
          .max_model_calls = 4,
          .max_tool_calls = 4,
          .max_tool_rounds = 1,
        },
      },
    },
    {
      .callbacks = {
        .on_error = [&](const reasoning::reasoning_error& error) {
          saw_error = error.code ==
                        reasoning::reasoning_error_code::tool_round_budget_exceeded &&
                      error.underlying_error ==
                        agent::make_error_code(
                          agent::llm_error_code::agent_loop_budget_exceeded);
        },
      },
    });

  require(!result.completed, "tool round budget should fail reasoning");
  require(result.reasoning_error ==
      reasoning::reasoning_error_code::tool_round_budget_exceeded,
    "reasoning should expose a stable tool-round budget error");
  require(result.underlying_error ==
      agent::make_error_code(agent::llm_error_code::agent_loop_budget_exceeded),
    "reasoning should preserve the underlying agent-loop budget error");
  require(result.error.find("tool round budget") != std::string::npos,
    "reasoning should expose a clear developer message");
  require(result.usage.tool_rounds == 1, "reasoning should record used tool rounds");
  require(result.usage.max_tool_rounds == 1, "reasoning should record max tool rounds");
  require(result.final_response.stop_reason == "tool_round_budget_exceeded",
    "reasoning should preserve the runtime stop reason");
  require(result.final_response.metadata.at("last_tool_call") == "echo_tool",
    "reasoning should preserve last tool call metadata");
  require(result.trace.back().metadata.at("stop_reason") == "tool_round_budget_exceeded",
    "terminal trace should include stop reason metadata");
  require(saw_error.load(), "reasoning should invoke on_error with stable code");

  const auto json = reasoning::reasoning_result_to_json(result);
  require(json.at("reasoning_error") == "tool_round_budget_exceeded",
    "reasoning JSON should export the stable error string");
  require(json.at("usage").at("tool_rounds") == 1,
    "reasoning JSON should export used tool rounds");
  require(json.at("usage").at("max_tool_rounds") == 1,
    "reasoning JSON should export max tool rounds");
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
    run("async run reports deltas and done", async_run_reports_deltas_and_done);
    run("async run can be cancelled", async_run_can_be_cancelled);
    run("llm errors are mapped to reasoning errors", llm_errors_are_mapped_to_reasoning_errors);
    run("reflect mode preserves model error details", reflect_mode_preserves_model_error_details);
    run("policy selector and trace json are stable", policy_selector_and_trace_json_are_stable);
    run("react mode uses tool provider", react_mode_uses_tool_provider);
    run("react mode maps agent stream events to reasoning events",
      react_mode_maps_agent_stream_events_to_reasoning_events);
    run("default agentic runner wires standard capabilities",
      default_agentic_runner_wires_standard_capabilities);
    run("simple mode with tool provider does not execute tools",
      simple_mode_with_tool_provider_does_not_execute_tools);
    run("reflect and retry retries until reflection passes",
      reflect_and_retry_retries_until_reflection_passes);
    run("model budget stops retry before second model call",
      model_budget_stops_retry_before_second_model_call);
    run("tool budget stops tool before invocation", tool_budget_stops_tool_before_invocation);
    run("tool round budget maps to stable reasoning error",
      tool_round_budget_maps_to_stable_reasoning_error);
    run("plan execute mode delegates to planning", plan_execute_mode_delegates_to_planning);
  }
  catch (const std::exception& ex) {
    println("[FAIL] {}", ex.what());
    return 1;
  }

  return 0;
}
