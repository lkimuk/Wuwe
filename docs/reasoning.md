# Reasoning

Use `<wuwe/agent/reasoning/reasoning.hpp>` as the module entry header.

Reasoning is Wuwe's strategy layer for standard agentic reasoning patterns. It
sits above the lower-level runtime modules and gives applications a single,
policy-driven facade for choosing how an agent should think, act, review, and
execute work.

The current implementation is intentionally framework-level: reasoning behavior
is not pushed into examples or application code. Applications choose a strategy
and budget; Wuwe coordinates model calls, tool use, planning, reflection,
events, trace records, and usage accounting.

## Current Status

Implemented:

- `simple`: one model call without tools.
- `react`: ReAct-style model/tool/model loops through `llm_agent_runner`.
- `reflect_and_retry`: model output reviewed by Reflection, retried when
  critique requests retry.
- `plan_execute`: explicit planning and step execution through Planning.
- Unified event stream across model, tool, reflection, and planning activity.
- Structured trace records stored on `reasoning_result`.
- JSON export helpers for usage, trace records, trace arrays, and full results.
- Usage counters for model calls, tool calls, reflection calls, and plan steps.
- Reasoning budgets for model calls, tool calls, tool rounds, reflection
  attempts, plan steps, and timeout.
- Tool budget enforcement before tool invocation.
- Model budget enforcement before real provider calls, including follow-up
  model calls inside ReAct loops.
- `reasoning_runner::run_async()` with a `reasoning_run` handle,
  `request_stop()`, `wait()`, and `get()`.
- Formal run callbacks for events, content deltas, completion, errors, and
  cancellation.
- Stable `reasoning_error_code` values with underlying LLM/provider error
  preservation.
- Agent tool-round exhaustion is mapped to
  `reasoning_error_code::tool_round_budget_exceeded` instead of leaking
  generic standard-library resource messages.
- Lightweight policy selection through `select_policy(...)`.
- Default agentic runner builders that assemble reasonable Planning,
  Reflection, Memory, Tool, and LLM components.
- Knowledge-aware runner builder that exposes `search_knowledge` as a standard
  reasoning tool.
- Qt-friendly public headers: internal public methods no longer use `emit(...)`
  as an API name.
- Public umbrella include through `<wuwe/wuwe.h>`.
- Dedicated example and tests.

Not implemented yet:

- token and monetary cost budgets,
- Tree-of-Thought or search-based reasoning,
- multi-agent debate or critique,
- retrieval-specific research loops,
- prompt-template registry for reasoning modes.

These are future enhancements, not required for the current foundation to be
usable.

## Module Boundary

The Reasoning module owns:

- `reasoning_mode`: the high-level strategy to run.
- `reasoning_budget`: the limits applied to a run.
- `reasoning_policy`: strategy, budget, streaming, replanning, and revision
  behavior.
- `reasoning_request`: user input, model settings, rubric, and metadata.
- `reasoning_result`: final content plus optional model, plan, reflection,
  step, trace, usage, error, and elapsed-time records.
- `reasoning_event`: the live event stream for host applications.
- `reasoning_runner`: the facade that coordinates lower-level Wuwe modules.

The Reasoning module does not own:

- model provider protocols,
- SSE or provider streaming parsers,
- tool schema reflection,
- tool implementation,
- plan validation or execution internals,
- reflection scoring internals,
- memory storage,
- UI display policy,
- persistence format for trace archives.

Those stay in their existing modules. Reasoning composes them.

## Public API

The primary types live in `wuwe::agent::reasoning`:

```cpp
enum class reasoning_mode {
  simple,
  react,
  reflect_and_retry,
  plan_execute,
};

struct reasoning_budget {
  std::size_t max_steps { 8 };
  std::size_t max_model_calls { 8 };
  std::size_t max_tool_calls { 16 };
  std::size_t max_tool_rounds { 4 };
  std::size_t max_reflection_attempts { 2 };
  std::chrono::milliseconds timeout { 0 };
};

struct reasoning_policy {
  reasoning_mode mode { reasoning_mode::react };
  reasoning_budget budget;
  bool enable_streaming { true };
  bool enable_reflection { false };
  bool allow_replanning { false };
  bool return_revised_output { true };
  planning::plan_policy planning;
  reflection::reflection_policy reflection;
};
```

`reasoning_result` returns the final answer plus audit data:

```cpp
struct reasoning_result {
  reasoning_mode mode;
  bool completed;
  std::string content;
  llm_response final_response;
  std::optional<planning::plan_run_result> plan;
  std::vector<reflection::reflection_run_result> reflections;
  std::vector<reasoning_step> steps;
  std::vector<reasoning_trace_record> trace;
  reasoning_usage usage;
  reasoning_error_code reasoning_error;
  std::error_code underlying_error;
  std::error_code error_code;
  std::string error;
  std::chrono::milliseconds elapsed;
};
```

`error_code` uses Wuwe's stable Reasoning error category. `underlying_error`
preserves provider-level failures, such as `llm_error_code::missing_api_key`,
so host applications can display precise diagnostics without parsing strings.

Policy selection helpers are also available:

```cpp
auto policy = reasoning::select_policy({
  .input = "Create a multi-step workflow for this task.",
  .has_tools = true,
});
```

The selector is intentionally lightweight. It gives host applications a sane
default, while still allowing them to set `reasoning_policy` explicitly for
product-specific behavior.

## Supported Modes

### `simple`

Runs a single model call and does not execute tools. This remains true even if
the runner was constructed with a tool provider.

Use it for:

- direct answers,
- classification,
- summarization,
- one-pass generation,
- tests where tool side effects must not happen.

### `react`

Runs through `llm_agent_runner` with optional tools. The loop is:

1. call the model,
2. inspect tool calls,
3. invoke allowed tools,
4. append tool observations,
5. call the model again,
6. stop when the model returns final content or the tool-round budget is hit.

Wuwe does not expose hidden chain-of-thought text. Applications receive
structured events for observable actions and observations instead.

### `reflect_and_retry`

Runs a model pass, sends the candidate output to `reflection_runner`, and then
acts on the reflection result:

- `pass`: return the candidate.
- `retry`: build a retry prompt from reflection issues and call the model again.
- `revise`: return `revised_output` when configured to do so.
- `block`, `escalate`, or `replan`: stop with an error for the caller.

Use it for:

- quality gates,
- self-correction,
- JSON or schema recovery,
- safety or policy checks,
- high-stakes output review.

### `plan_execute`

Delegates to `planning::plan_runner`. This mode is for explicit
plan-and-execute workflows with plan validation, dependency-aware step
execution, retry, replanning, approval gates, checkpointing, and plan events.

Use it when work is naturally multi-step and the application wants an explicit
plan object rather than an implicit tool loop.

## Usage Examples

Simple model call:

```cpp
namespace reasoning = wuwe::agent::reasoning;

reasoning::reasoning_runner runner(client);
auto result = runner.run({
  .input = "Explain RAII briefly.",
  .policy = {
    .mode = reasoning::reasoning_mode::simple,
  },
});
```

ReAct with tools:

```cpp
auto provider = std::make_shared<wuwe::tool_provider<get_weather>>();
auto runner = reasoning::reasoning_runner::with_tools(client, provider);

auto result = runner.run({
  .input = "What's the weather in Tokyo?",
  .policy = {
    .mode = reasoning::reasoning_mode::react,
    .budget = {
      .max_model_calls = 3,
      .max_tool_calls = 1,
      .max_tool_rounds = 2,
    },
    .enable_streaming = true,
  },
});
```

Plan execution:

```cpp
reasoning::reasoning_runner runner({
  .planner = planner,
  .executor = executor,
});

auto result = runner.run({
  .input = "Inspect and summarize this project.",
  .policy = {
    .mode = reasoning::reasoning_mode::plan_execute,
    .budget = {
      .max_steps = 8,
    },
  },
});
```

Default agentic runner:

```cpp
auto provider = std::make_shared<wuwe::tool_provider<get_weather>>();
auto runner = reasoning::make_default_agentic_runner(client, provider, {
  .model = "openai/gpt-4.1-mini",
  .memory = &memory,
});
```

This constructs the common pieces ReArk and other host applications otherwise
have to repeat: ReAct tool execution, a default Reflection runner, a Planning
runner, optional Memory, and the unified Reasoning event/trace contract.

Knowledge-aware runner:

```cpp
auto runner = reasoning::make_knowledge_aware_runner(client, retriever, {
  .model = "openai/gpt-4.1-mini",
});
```

The runner exposes Knowledge Retrieval through the standard `search_knowledge`
tool, so applications do not need to hand-roll the RAG tool bridge for every
integration.

Async execution:

```cpp
reasoning::reasoning_run_options options;
options.callbacks.on_delta = [](std::string_view delta) {
  std::cout << delta << std::flush;
};
options.callbacks.on_done = [](const reasoning::reasoning_result& result) {
  // Persist result.trace or update host state.
};

auto run = runner.run_async({
  .input = "Analyze this document set.",
  .policy = reasoning::select_policy(reasoning::reasoning_task_profile::complex_analysis),
}, std::move(options));

// Later, from host cancellation:
run.request_stop();
auto result = run.get();
```

Callbacks execute on the runner execution thread. GUI hosts such as ReArk
should marshal callbacks back to the UI thread before updating UI state.

## Events

`reasoning_observer` receives one stream across all supported modes:

- `started`
- `model_started`
- `content_delta`
- `tool_started`
- `tool_completed`
- `reflection_started`
- `reflection_completed`
- `plan_created`
- `plan_step_started`
- `plan_step_completed`
- `plan_step_failed`
- `plan_step_blocked`
- `plan_revised`
- `completed`
- `failed`
- `cancelled`

Host applications can use this stream for:

- progress UIs,
- live streaming display,
- logs,
- telemetry,
- debugging,
- integration tests.

For async runs, `reasoning_callbacks` separates common host needs:

- `on_event`: every observable event.
- `on_delta`: streamed or synthesized content deltas.
- `on_done`: successful terminal result.
- `on_error`: failed terminal result with stable `reasoning_error_code` and
  optional underlying provider error.
- `on_cancelled`: cancelled terminal result.

Terminal callbacks are dispatched after the final trace and usage are attached
to `reasoning_result`, so UI timelines and logs can consume the final result
without reconstructing trace state from earlier events.

## Trace Records

Every emitted event is also recorded in `reasoning_result::trace`.

Each trace record includes:

- `sequence`: stable one-based event order,
- `type`: event type,
- `mode`: reasoning mode that produced the event,
- `step_id`: planning step id when available,
- `message`: human-readable event message,
- `delta`: streamed content delta when available,
- `error`: terminal error text for failed or cancelled events,
- `elapsed`: elapsed time since the reasoning run started,
- `metadata`: caller or subsystem metadata.

Trace records are for observable execution. They are not hidden
chain-of-thought. Wuwe records lifecycle events, actions, tool observations,
planning events, reflection events, deltas, and terminal status.

Typical use:

```cpp
for (const auto& record : result.trace) {
  std::cout << record.sequence << " "
            << reasoning::to_string(record.type) << " "
            << record.message << "\n";
}
```

JSON export helpers:

```cpp
auto trace_json = reasoning::reasoning_trace_to_json(result.trace);
auto usage_json = reasoning::reasoning_usage_to_json(result.usage);
auto result_json = reasoning::reasoning_result_to_json(result);
```

These helpers are intended for UI timelines, debug panes, telemetry,
session-replay storage, and golden-trace regression tests.

## Usage Counters

`reasoning_result::usage` currently tracks:

- `model_calls`: provider calls that were actually allowed to start,
- `tool_calls`: tool calls that were actually allowed to start,
- `tool_rounds`: ReAct model/tool rounds consumed before completion or
  terminal failure,
- `max_tool_rounds`: configured ReAct round budget for the run,
- `reflection_calls`: reflection reviews that were actually started,
- `plan_steps`: planning steps started or reported by Planning.

Usage counters are intentionally separate from trace. Trace answers "what
happened and in what order"; usage answers "how much work was consumed."

## Budgets

`reasoning_budget` is enforced by the framework:

- `max_model_calls`: limits real provider calls, including follow-up model
  calls inside ReAct loops.
- `max_tool_calls`: limits tool execution before the tool is invoked.
- `max_tool_rounds`: limits ReAct tool/model rounds in `llm_agent_runner`.
  Exhaustion returns `tool_round_budget_exceeded` with last tool/model
  metadata instead of a generic resource error.
- `max_reflection_attempts`: limits reflection reviews in
  `reflect_and_retry`.
- `max_steps`: limits plan execution steps.
- `timeout`: cancels the reasoning run when non-zero and exceeded.

Numeric maxima are literal upper bounds. For example:

- `max_tool_calls = 0` means tools are not allowed.
- `max_model_calls = 1` means only one provider call is allowed.
- `timeout = 0` means no reasoning-level timeout.

Tool budgets are checked before invoking the tool. This matters because tools
may have side effects.

Model budgets are checked immediately before calling the provider. This means
ReAct follow-up calls are counted, not just the outer reasoning call.

## Cancellation

`reasoning_runner_options::should_cancel` provides cooperative cancellation at
the reasoning layer. Cancellation is also passed down to Planning through
`plan_runner` and to model/tool loops through `llm_agent_runner` behavior.
For Plan/Execute workflows, `tool_plan_executor` forwards the run stop token to
providers that implement `invoke(name, arguments_json, stop_token)`.

A cancelled run returns:

- `completed = false`,
- `reasoning_error = reasoning_error_code::cancelled`,
- `error_code = reasoning::make_error_code(reasoning_error_code::cancelled)`,
- a terminal `cancelled` or `failed` event,
- trace records up to the stop point.

`reasoning_runner::run_async()` combines the caller-provided
`reasoning_run_options::stop_token` with the returned handle's
`request_stop()`. Either source can cancel the run.

## Extension Points

`reasoning_runner_options` supports advanced composition:

- `client`: direct LLM client path.
- `agent_complete`: custom model/tool completion function.
- `planner` and `executor`: Planning integration.
- `plan_store`: optional Planning persistence.
- `memory`: optional Memory integration.
- `reflection`: Reflection integration.
- `observer`: live reasoning event sink.
- `should_cancel`: cooperative cancellation hook.

`make_default_agentic_runner(...)` and `make_knowledge_aware_runner(...)`
cover the common production composition path. Use the lower-level
`reasoning_runner_options` constructor when the host application needs custom
planner, executor, reflection, memory, or model/tool completion behavior.

When using a custom `agent_complete`, the custom implementation should honor
the supplied `llm_agent_run_options.callbacks`. Reasoning-level model and tool
budgets rely on the lower-level callbacks:

- call `callbacks.on_model_start(request)` before every provider call,
- call `callbacks.allow_tool_call(call)` before invoking a tool,
- emit deltas, tool lifecycle events, cancellation, errors, and completion as
  appropriate.

The built-in direct-client and `with_tools()` paths already do this.

## Design Decisions

### Reasoning Is A Facade

Reasoning does not duplicate Planning, Reflection, Memory, Tools, or Streaming.
It coordinates them under a single policy and result contract.

This keeps lower-level modules usable directly while giving applications a
standard high-level entry point.

### No Hidden Chain-Of-Thought Exposure

The framework exposes structured progress, actions, observations, and terminal
state. It does not promise access to private model reasoning text. This keeps
the public API provider-neutral and appropriate for production logging.

### Budgets Belong In The Framework

Applications should not have to reimplement call counting and guardrails for
every strategy. Wuwe enforces the budget at the layer where the work is
coordinated.

### Trace Is A Result Artifact

Live events are useful for UI. Persisted trace is useful for tests, telemetry,
and debugging. Both are generated from the same event stream so they stay
consistent.

## Tests

`reasoning_tests` covers:

- simple one-pass model reasoning,
- async reasoning callbacks, terminal callback behavior, and handle
  cancellation,
- simple mode created with `with_tools()` not executing tools,
- ReAct-style tool use through a tool provider,
- reflect-and-retry with critique feedback,
- reflect-and-retry event mode labeling,
- stable Reasoning error mapping from underlying LLM errors,
- policy selection and JSON trace export helpers,
- default agentic runner construction,
- model-call budget enforcement,
- tool-call budget enforcement before invocation,
- tool-round budget exhaustion mapping and JSON usage export,
- structured trace records,
- usage counters,
- plan execution through Planning.

Run:

```powershell
cmake --build build --config Debug --target reasoning_tests
ctest --test-dir build -C Debug -R reasoning_tests --output-on-failure
```

Related regression set:

```powershell
cmake --build build --config Debug --target agent_runtime_tests planning_tests reflection_tests reasoning_tests reasoning_example
ctest --test-dir build -C Debug -R "agent_runtime_tests|planning_tests|reflection_tests|reasoning_tests" --output-on-failure
```

## Example

`reasoning_example` demonstrates:

- deterministic offline plan execution,
- live OpenRouter ReAct reasoning,
- content streaming,
- tool lifecycle events,
- usage and trace counts.

Run:

```powershell
cmake --build build --config Debug --target reasoning_example
.\build\examples\Debug\reasoning_example.exe
```

Set `OPENROUTER_API_KEY` to run the live model portion.

## Known Boundaries

The current implementation is deliberately complete for the first framework
version, but these boundaries remain:

- There is no token-level or cost-level budget yet.
- Custom `agent_complete` implementations must honor callbacks for budgets and
  trace fidelity.
- `reflect_and_retry` uses a generic retry prompt rather than a prompt-template
  registry.
- `plan_execute` relies on Planning for detailed plan semantics and does not
  duplicate plan validation.
- `run_async` owns scheduling through `std::jthread`; GUI hosts must still
  marshal callbacks onto their UI thread.

## Future Enhancements

High-value next steps:

- Token and cost budgets based on provider usage metadata.
- JSON deserialization for `reasoning_trace_record`.
- Standard trace sinks for files, telemetry, and test snapshots.
- Configurable retry prompt templates for Reflection.
- Per-mode default policies, such as conservative ReAct defaults or strict JSON
  recovery defaults.
- Better metadata propagation from tools, plan observations, and reflection
  issues into trace records.
- Budget-aware stop reasons as a typed enum instead of only error strings.

Advanced strategy additions:

- Tree-of-Thought or graph search over candidate answers.
- Best-of-N sampling with reflection ranking.
- Debate or multi-agent critique.
- Research loops specialized for retrieval and source grounding.
- Cost-aware inference scaling policies.
- Human approval hooks at the Reasoning layer, coordinated with Planning.

Operational enhancements:

- Trace redaction for sensitive tool outputs.
- Trace sampling controls for production workloads.
- Runtime metrics integration.
- Policy presets loaded from configuration.
- Golden-trace regression tests for complex workflows.

## Review Checklist

Before changing this module, check:

- Does the change preserve `simple` as no-tool execution?
- Are all real provider calls counted against `max_model_calls`?
- Are tools blocked before invocation when over budget?
- Does every emitted event also appear in `result.trace`?
- Does the terminal trace event match the returned result status?
- Are new modes documented and tested?
- Does custom composition still work through `reasoning_runner_options`?
- Did related tests pass: Agent Runtime, Planning, Reflection, and Reasoning?
