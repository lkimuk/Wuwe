# ReArk Integration Notes

This note summarizes the Wuwe packaging and document-parser runtime changes
that ReArk should account for when updating its Wuwe integration.

## What Changed

Wuwe now ships as one complete package named `wuwe`.

The older split-package model (`wuwe-core` and `wuwe-full`) is intentionally no
longer the public release path. The package includes the C++ SDK, examples,
documentation, and bundled runtime sidecars needed by the default RAG document
loader.

The document parser is now an implementation detail. ReArk should not ask users
to configure Apache Tika, Java, a parser URL, or a runtime directory.

## Recent Wuwe Changes ReArk Should Notice

### Agent Runtime

Wuwe now has embeddable agent runtime APIs for host applications. ReArk should
prefer the runtime runner APIs when embedding agent behavior instead of wiring
LLM calls, tools, streaming, cancellation, and final-result callbacks by hand.

Relevant capabilities:

- synchronous and async agent execution,
- cooperative cancellation with `std::stop_token`,
- structured Agent event callbacks through `llm_agent_callbacks::on_event`,
- raw normalized LLM stream callbacks through
  `llm_agent_callbacks::on_stream_event`,
- tool lifecycle callbacks,
- final-result callbacks,
- stateful tool providers whose private state stays outside the model-visible
  schema.

### OpenAI-Compatible Streaming

The LLM layer now supports true OpenAI-compatible streaming and shared
streaming contracts across built-in providers. ReArk can render partial model
output as it arrives instead of waiting for the final response.

The Agent runner no longer reduces streaming to text-only `content_delta`
callbacks. ReArk can observe:

- raw normalized `llm_stream_event` values, including `reasoning_delta`,
  `reasoning_done`, and `tool_call_delta`,
- Agent-level `model_first_event`,
- Agent-level `model_content_delta`,
- Agent-level `model_reasoning_delta`,
- Agent-level `model_reasoning_completed`,
- Agent-level `tool_call_building`,
- Agent-level `tool_call_ready`,
- Agent-level `tool_started` and `tool_completed`,
- Agent-level `model_completed`, `agent_completed`, `agent_failed`, and
  `agent_cancelled`.

Integration expectation:

- use streaming callbacks/event handling for interactive chat or long-running
  agent tasks,
- render `reasoning_delta` as provider-supplied visible analysis summary when
  present,
- keep `content_delta` reserved for final answer Markdown/text,
- show user-friendly Agent phases such as waiting for model, preparing a tool
  call, executing a tool, and generating the final answer,
- do not treat partial Markdown content such as headings or tables as thinking
  progress,
- do not prompt models to write fake "I am thinking" status text in the final
  answer body,
- do not display raw streamed tool argument JSON directly unless ReArk makes an
  explicit product/security decision to reveal it,
- preserve cancellation behavior during streaming,
- avoid building a separate ad hoc streaming parser in ReArk when Wuwe already
  exposes the runtime surface.

Wuwe streaming clients now also accept staged timeout budgets through
`llm_client_config::stream_timeouts`:

- `total_ms`,
- `connect_ms`,
- `first_event_ms`,
- `idle_ms`.

Parser-observed first-event and idle timeouts return
`llm_error_code::timeout` with `timeout_phase` and `timeout_ms` metadata.
Transport-level timeouts are still classified as timeouts, but phase metadata is
only available when the LLM streaming layer observes the phase boundary.

### Reasoning Summary Stream

Wuwe now exposes provider-supplied visible reasoning summaries separately from
final answer content:

```cpp
enum class llm_stream_event_type {
  content_delta,
  reasoning_delta,
  reasoning_done,
  tool_call_delta,
  tool_call_done,
  done,
  error,
};
```

`llm_response` also carries:

```cpp
std::string reasoning_summary;
std::map<std::string, std::string> reasoning_metadata;
```

This is a display channel for summaries or exposed thinking fields that the
provider actually returns. It is not a request to expose hidden
chain-of-thought, and Wuwe does not fabricate reasoning text when a provider or
model does not provide it.

Recommended ReArk behavior:

- When `reasoning_delta` is present, show it as temporary "analysis" progress
  and keep streaming `content_delta` into the final-answer renderer.
- On `reasoning_done`, either collapse the summary, keep it in a timeline, or
  replace the temporary analysis surface with the final answer.
- When no reasoning events are present, show real lifecycle states such as
  waiting for model, preparing tool call, executing tool, and generating final
  answer.
- Never render raw `content_delta` Markdown as if it were thinking progress.
- Never hard-code fake thinking text in ReArk to make unsupported providers
  appear to reason.

### LLM Provider Registry And Factory

Wuwe now exposes a narrow provider registry and factory surface for host
applications. ReArk should use these headers for model-provider settings and
provider construction:

```cpp
#include <wuwe/agent/llm/llm_provider_registry.h>
#include <wuwe/agent/llm/llm_provider_factory.h>
```

Use `list_llm_providers()`, `find_llm_provider()`,
`make_default_llm_config()`, and `normalize_llm_client_config()` to populate UI
state and apply Wuwe-owned defaults. Use `make_llm_client()` or
`llm_client_factory` to construct the selected provider.

Provider metadata includes both default base URLs and provider-specific
chat-completions paths, so ReArk should not maintain a parallel provider URL
table. The built-in OpenAI-compatible presets include `Zhipu` for Zhipu
GLM/BigModel, with base URL `https://open.bigmodel.cn/api/paas/v4`, chat path
`/chat/completions`, and API key environment names `ZHIPU_API_KEY` then
`BIGMODEL_API_KEY`.

Provider capabilities include `reasoning_summary` and
`streaming_reasoning_summary`. ReArk can use these as UI hints, but actual
rendering should still be driven by the stream events because support may vary
by model under the same provider.

ReArk should avoid including `<wuwe/wuwe.h>` in provider-selection translation
units. The aggregation header remains available for convenience, but Wuwe no
longer performs LLM factory registration from that public header. This keeps
host code from pulling registration side effects into arbitrary translation
units.

### Reasoning Facade

Wuwe now includes a policy-driven Reasoning facade for selecting standard
agentic reasoning patterns over the lower-level runtime modules.

Available strategy families include:

- simple single-pass execution,
- ReAct-style tool use,
- reflect-and-retry self-correction,
- plan-and-execute workflows.

ReArk should use the Reasoning facade when it wants a higher-level agentic
workflow rather than manually coordinating prompts, tools, reflection, and plan
steps.

The facade emits one unified event stream and returns structured traces with
budget usage counters, which are useful for ReArk UI timelines, logs, debugging,
and audit views.

When streaming is enabled, the Reasoning facade now preserves model stream
progress instead of only forwarding text deltas. ReArk can listen for
`model_first_event`, `content_delta`, `reasoning_delta`,
`reasoning_completed`, `tool_call_building`, `tool_call_ready`, `tool_started`,
`tool_completed`, and `model_completed` from the same reasoning event stream it
already uses for workflow progress.

Wuwe Reasoning now exposes a native async runner API. ReArk should prefer
`reasoning_runner::run_async(...)` for interactive UI flows instead of wrapping
`reasoning_runner::run(...)` in its own compatibility bridge.

Relevant async capabilities:

- `reasoning_run::request_stop()`,
- `reasoning_run::wait()`,
- `reasoning_run::get()`,
- callback hooks for `on_event`, `on_delta`, `on_reasoning_delta`,
  `on_reasoning_done`, `on_done`, `on_error`, and `on_cancelled`,
- cancellation through either a caller-provided `std::stop_token` or the run
  handle,
- stable `reasoning_error_code` values with underlying LLM/provider errors
  preserved on the result.

Agent/tool loop exhaustion is now modeled explicitly. If a ReAct run reaches
`max_tool_rounds` before the model produces a final answer, Wuwe returns:

- LLM runtime: `llm_error_code::agent_loop_budget_exceeded`,
- Reasoning facade: `reasoning_error_code::tool_round_budget_exceeded`,
- `stop_reason = "tool_round_budget_exceeded"`,
- usage fields `tool_rounds` and `max_tool_rounds`,
- trace/final-response metadata for `last_tool_call`, `last_tool_result`, and
  `last_model_response`.

ReArk should key UI behavior off those stable codes instead of displaying
`std::error_code::message()`. The old misleading text
`resource unavailable try again` should no longer be surfaced for tool-loop
budget exhaustion.

Reasoning events, streaming deltas, trace updates, and final results are still
emitted from the runner's execution thread. ReArk should marshal those
callbacks back to its UI thread before updating UI state.

ReArk should also prefer Wuwe's default builders for common workflows:

```cpp
auto runner = wuwe::agent::reasoning::make_default_agentic_runner(
  client,
  provider,
  {
    .model = model,
    .memory = memory,
  });
```

For RAG-backed reasoning, prefer:

```cpp
auto runner = wuwe::agent::reasoning::make_knowledge_aware_runner(
  client,
  retriever,
  {
    .model = model,
    .memory = memory,
  });
```

These helpers reduce ReArk-side glue for ReAct, Reflection, Planning, Memory,
Knowledge Retrieval, streaming, cancellation, trace, and budget reporting.

Reasoning traces and usage counters can now be exported with:

```cpp
auto trace_json = wuwe::agent::reasoning::reasoning_trace_to_json(result.trace);
auto usage_json = wuwe::agent::reasoning::reasoning_usage_to_json(result.usage);
auto result_json = wuwe::agent::reasoning::reasoning_result_to_json(result);
```

ReArk can now compose its own Hyle/source/decompile/signature provider with the
standard Wuwe knowledge provider instead of writing a hand-rolled forwarding
bridge:

```cpp
auto tools = wuwe::compose_tool_providers(reark_tools, knowledge_tools);
auto runner = wuwe::agent::reasoning::make_default_agentic_runner(
  client,
  tools,
  options);
```

Provider order is the override policy: when two providers expose the same tool
name, the earlier provider wins. This lets ReArk keep product-specific tools in
front while still using Wuwe's standard `search_knowledge` provider.

The composed provider remains cancellation-aware. In both ReAct-style runs and
Plan/Execute tool steps, Wuwe forwards the active `std::stop_token` to providers
that implement `invoke(name, arguments_json, stop_token)`.

Public Wuwe headers no longer expose `emit(...)` as a method name in the
Reasoning/Planning/Reflection event path, reducing Qt macro friction for ReArk.

### Controlled Local Execution For ReArk Agents

Wuwe now has the first baseline of a controlled local execution runtime that
ReArk can use for bounded Python-based reverse-engineering calculations. This
is intended for tasks such as decoding bytes, testing crypto hypotheses,
transforming constants, validating generated scripts, and returning
stdout/stderr to the agent for follow-up analysis.

The default execution path is still not a strong OS sandbox. The default
backend is `controlled_process`: ReArk chooses the Python interpreter, and Wuwe
validates or executes that host-selected interpreter with controlled
environment, working directory, stdin/stdout/stderr, timeout, cancellation, and
process lifecycle. Wuwe does not claim `controlled_process` enforces complete
network or filesystem isolation against code running with the current OS user
privileges.

For this backend, `network: false` and `file_write: false` mean the model cannot
ask the tool to enable those capabilities and Wuwe will not treat them as
approved capabilities. They are not kernel-enforced Python restrictions. ReArk
should keep the workdir task-local, pass selected bytes/text through
`stdin_text`, and avoid placing unrelated sensitive files in locations reachable
by the execution user until Wuwe provides a backend that advertises explicit
network and filesystem restriction features.

Available Wuwe pieces:

- `wuwe::agent::capability`: stable capability names such as
  `process.python`, `process.shell`, `filesystem.write`, and
  `network.outbound`.
- `wuwe::agent::approval`: host approval request and decision contracts for
  high-risk capability escalation.
- `wuwe::agent::audit`: structured audit events and sinks for attempted,
  denied, approved, started, completed, failed, timed-out, and cancelled
  executions.
- `wuwe::agent::sandbox`: explicit isolation levels including
  `controlled_process`, `restricted_process`, `container`, and `wasm`.
- `wuwe::agent::execution`: request/result/policy/runtime/backend contracts,
  `controlled_process_backend`, explicit restricted backend registration, and
  `execution_tool_provider`.

The first backend supports Windows Python snippets through `CreateProcessW`
without invoking a system shell. It writes the snippet to a temporary script in
the execution workdir, passes optional stdin through a pipe, captures
stdout/stderr with byte limits, marks truncation, supports timeout/cancel, and
returns structured launch, backend, policy, input-limit, and approval failures.
The Windows implementation uses a restricted process handle inheritance list so
the child receives only its standard IO handles, not arbitrary inheritable
handles from the host process. When Job Objects are enabled, timeout and
cancellation terminate the process tree, and Windows enforces configured
process count, CPU time, and memory limits.

The first agent-facing tool is intentionally narrow:

```text
run_python_snippet(code, stdin_text?, timeout_ms?)
```

ReArk should expose a product-specific name such as:

```text
run_analysis_script(code, stdin_text?, timeout_ms?)
```

The model must not be allowed to set permissions, environment variables,
interpreter paths, network access, file-write access, or shell commands through
tool arguments. ReArk should configure those through host-owned
`execution_policy`.

Wuwe now provides interpreter diagnostics for ReArk's resolver/settings UI:

```cpp
auto probe = execution::probe_python_interpreter({
  .interpreter = reark_python_path,
  .workdir = reark_analysis_temp_dir,
  .env = {},
  .timeout = std::chrono::milliseconds(3000),
});
```

ReArk still owns Python discovery policy, such as bundled Python, user-selected
Python, virtualenv, conda, `py` launcher, or PATH lookup. Wuwe does not search
or choose among those options. Wuwe probes the interpreter ReArk passes in,
using no-shell launch, stdout/stderr capture, timeout, and structured status
values such as `ok`, `not_found`, `not_executable`, `permission_denied`,
`startup_timeout`, `invalid_python`, and `unsupported_version`.

When `controlled_process_backend` cannot launch Python, execution results now
include structured metadata such as:

```text
error_code
launch_error_code
launch_error_message
python_interpreter
timeout_phase
```

ReArk should key user-facing setup diagnostics off those stable values instead
of parsing `error_message`.

Recommended ReArk policy for the first integration:

```text
language: python
backend: controlled_process
tool name: run_analysis_script
shell: false
network: false
file_write: false
env: empty or explicit allowlist only
timeout: 3000-5000 ms
stdout/stderr: 65536 bytes each
max_code_bytes: 65536
max_stdin_bytes: 1048576
max_total_input_bytes: 1114112
max_arguments_bytes: 131072
max_process_count: 8
max_memory_bytes: host-selected, 0 means no Job Object memory limit
max_cpu_time: host-selected, 0 means no Job Object CPU-time limit
workdir: ReArk analysis temp directory
stdin: selected bytes, constants, resource data, or decompiled text
```

Example integration shape:

```cpp
namespace execution = wuwe::agent::execution;

auto execution_runtime = std::make_unique<execution::execution_runtime>(
  execution::make_controlled_process_backend({
    .python_interpreter = reark_python_path,
    .fallback_workdir = reark_analysis_temp_dir,
  }),
  execution::execution_policy {
    .allowed_languages = { execution::execution_language::python },
    .default_workdir = reark_analysis_temp_dir,
    .max_limits = {
      .timeout = std::chrono::milliseconds(5000),
      .max_stdout_bytes = 65536,
      .max_stderr_bytes = 65536,
      .max_code_bytes = 65536,
      .max_stdin_bytes = 1048576,
      .max_total_input_bytes = 1114112,
      .max_process_count = 8,
    },
    .allow_network = false,
    .allow_file_read = false,
    .allow_file_write = false,
    .allow_shell = false,
    .allowed_env = {},
  },
  reark_audit_sink,
  reark_approval_service);

auto execution_tools =
  std::make_shared<execution::execution_tool_provider>(
    *execution_runtime,
    execution::execution_tool_options {
      .tool_name = "run_analysis_script",
      .description =
        "Run a short Python analysis script with bounded input, output, and timeout.",
      .max_arguments_bytes = 131072,
      .allow_empty_stdin = true,
      .allow_additional_arguments = false,
      .reject_timeout_outside_limits = true,
    });

auto tools = wuwe::compose_tool_providers(
  reark_tools,
  execution_tools,
  knowledge_tools);
```

Lifetime requirement: ReArk must keep `execution_runtime`, its backend, audit
sink, and approval service alive for as long as the composed tool provider can
be invoked. The tool provider stores a non-owning reference to the runtime.

The tool provider now exposes a policy-aligned JSON schema. The schema only
contains `code`, `stdin_text`, and `timeout_ms`, sets `additionalProperties` to
`false` by default, and publishes `maxLength`/`minimum`/`maximum` values from
the host policy and tool options. Provider-level validation rejects oversized
raw argument JSON before parsing, rejects unknown fields, and rejects timeout
hints outside the exposed bounds before the backend can launch.

ReArk owns:

- whether controlled execution is enabled,
- Python interpreter discovery, selection, resolver policy, and packaging,
- analysis workdir selection,
- whether to stay on `controlled_process` or explicitly opt into
  `restricted_process`,
- readable and writable root policy for restricted execution,
- selected input packaging into `stdin_text`,
- user approval UI,
- audit persistence policy,
- UI rendering of stdout/stderr, timeout, cancellation, and truncation.

Audit coverage now includes provider-level argument rejections such as
`arguments_limit`, `schema_invalid`, and `timeout_limit`, plus runtime policy
denials, approval decisions, launch/backend failures, timeout, cancellation,
and completion. Execution-finished audit events also include backend name,
isolation level, process/resource enforcement, and whether file/network deny is
enforced by the active backend. For `controlled_process`, file and network deny
remain `not_enforced`.

Wuwe's default execution backend registry now exposes backend availability and
selection by enforced capability. In the default registry, `controlled_process`
is available; `restricted_process`, `container`, and `wasm` remain visible as
unavailable slots. If ReArk requires enforced filesystem read/write denial or
network denial against the default registry, selection returns no backend
rather than silently downgrading to `controlled_process`.

Windows builds now also expose an explicit opt-in restricted-process path:

```cpp
execution::execution_backend_registry_options options;
options.enable_restricted_process_backend = true;
options.restricted_process.python_interpreter = reark_python_path;
options.restricted_process.fallback_workdir = reark_analysis_temp_dir;
options.restricted_process.readable_roots = reark_read_roots;
options.restricted_process.writable_roots = reark_write_roots;

auto registry = execution::make_execution_backend_registry(options);
```

When the configured Windows restricted backend is available, ReArk can require:

```cpp
execution::execution_backend_requirements requirements;
requirements.isolation = sandbox::isolation_level::restricted_process;
requirements.require_filesystem_read_deny = true;
requirements.require_filesystem_write_deny = true;
requirements.require_network_deny = true;
```

This path uses the Windows AppContainer-style restricted backend, no-shell
launch, staged minimal Python runtime, Job Object lifecycle/resource limits,
configured readable/writable roots, network denial, and audit/result metadata.
The backend remains default-off: ReArk must opt in only after it has chosen the
interpreter, workdir, readable roots, writable roots, approval UX, and result
handling policy. ReArk should still display Wuwe's enforcement metadata instead
of hard-coding claims about sandbox strength.

Wuwe owns:

- execution API contracts,
- policy evaluation and limit clamping,
- approval request shape,
- audit event shape,
- backend availability and requirement selection,
- host-selected Python interpreter probing and structured launch diagnostics,
- no-shell controlled process launch,
- stdin/stdout/stderr capture,
- timeout and cancellation,
- tool provider integration with ReAct and Planning.

## Longer-Term Execution Work

The following items are intentionally longer-term and should not block ReArk's
first controlled-execution integration, but they should remain visible in
planning:

- Productize ReArk's restricted execution UX: interpreter resolver, opt-in
  policy, readable/writable root selection, user-visible explanation, and audit
  persistence.
- Add broader Windows restricted-backend acceptance coverage for additional
  Python distributions, venv/conda layouts, long paths, non-ASCII paths, and
  multiple concurrent restricted runs.
- Decide whether the Windows restricted backend should remain explicit opt-in
  forever or become selectable by product policy in specific trusted
  deployments.
- Implement Linux and macOS process governance equivalents for timeout,
  cancellation, process-tree cleanup, CPU/memory limits, filesystem roots, and
  network denial.
- Implement at least one true P3 backend beyond Windows restricted process:
  container backend or WASM/WASI backend.
- Add packaging guidance for any restricted backend runtime sidecars that ReArk
  chooses to bundle.
- Keep `controlled_process` documentation explicit: it is useful for bounded
  computation, not a filesystem/network sandbox.

Initial ReArk verification:

- Register `run_analysis_script` and confirm the tool schema only exposes
  `code`, `stdin_text`, and `timeout_ms`.
- Run a small Python transform over selected hex/base64 input and verify stdout
  returns to the agent.
- Verify timeout returns `timed_out=true`.
- Verify cancellation returns `cancelled=true`.
- Verify long stdout/stderr are truncated and marked.
- Verify oversized `code`, oversized `stdin_text`, and oversized total input
  return `policy_denied` before Python starts.
- Verify no host environment variables are inherited unless explicitly
  allowlisted.
- Verify denied or failed attempts emit audit events.

Next Wuwe-side enhancements after ReArk feedback should be tracked separately:
broader Windows restricted-process hardening, Linux/macOS backend parity, then
container backend and WASM backend.

### Knowledge And URL Loading

The Knowledge Retrieval module now supports local files, directories, URL/HTML
loading, richer document parsing through the bundled runtime, cited chunking,
retrieval, grounding checks, and `search_knowledge` tool exposure.

ReArk should treat `knowledge_document_loader::make_default()` as the standard
ingestion entry point for mixed local document and URL workflows.

## Default Document Loading

Use the default loader with no parser arguments:

```cpp
auto loader = wuwe::agent::knowledge::knowledge_document_loader::make_default();
```

Do not use the old pattern:

```cpp
// Old integration style. Do not use.
auto loader =
  wuwe::agent::knowledge::knowledge_document_loader::make_default(tika_url);
```

The default loader automatically discovers the package-layout runtime directory
and starts the bundled parser when needed.

## Runtime Layout ReArk Should Ship

ReArk previously consumed Wuwe directly from the CMake install prefix. That
remains supported, and the install prefix now carries the bundled runtime too.

When ReArk packages or embeds Wuwe from an install directory, keep this layout
available next to the ReArk executable or its working directory:

```text
runtime/
  tika/
    tika-server-standard.jar
  jre/
    bin/
      java.exe
```

The runtime discovery path checks package-style `runtime/tika` locations next to
the current working directory and executable. If ReArk repackages files into its
own installer, it should preserve that `runtime` directory as a unit.

If ReArk points directly at a Wuwe install prefix during development, verify
that the prefix contains the same `runtime` directory:

```powershell
cmake --install build --config Release --prefix install
Test-Path install\runtime\tika\tika-server-standard.jar
Test-Path install\runtime\jre\bin\java.exe
```

## Removed User-Facing Configuration

ReArk should remove UI, config, docs, or launch scripts that mention:

- `WUWE_TIKA_URL`
- `WUWE_TIKA_RUNTIME_DIR`
- `WUWE_TIKA_AUTO_START`
- `--tika-url`
- manual Tika startup instructions
- choosing between `wuwe-core` and `wuwe-full`

These are no longer part of the simple user path.

## Packaging Command

Generate the Wuwe release package with:

```powershell
.\tools\package-wuwe.ps1 -Configuration Release
```

The expected output is:

```text
dist/wuwe-<version>-windows-x64.zip
```

The generated package manifest records the bundled parser and JRE checksums.

For direct install-prefix consumption, use:

```powershell
cmake --install build --config Release --prefix install
```

The install prefix is expected to be self-contained for ReArk development and
installer staging.

## Verification Signals

Before updating ReArk to a new Wuwe package, verify:

- `manifest.json` has `"name": "wuwe"`.
- `runtime/tika/tika-server-standard.jar` exists.
- `runtime/jre/bin/java.exe` exists on Windows x64 packages.
- The same runtime files exist when ReArk consumes a raw CMake install prefix.
- ReArk can create `knowledge_document_loader::make_default()` without any
  parser URL configuration.
- PDF or Office document ingestion works without asking the user to start or
  configure Tika.

## Intent

The integration goal is a zero-configuration user experience: ReArk users should
interact with document ingestion and RAG features, not with the parser runtime
behind them.
