# Controlled Local Execution Runtime

Status: P0/P1 controlled-process baseline implemented; P2/P3 backend contract
and selection surfaces implemented; stronger restricted, container, and WASM
execution backends remain future work.

Use `<wuwe/agent/execution/execution.hpp>` as the planned module entry header.

This document defines the long-term architecture for adding controlled local
execution to Wuwe agents. The first product driver is ReArk reverse-engineering
analysis, where an agent often needs deterministic computation such as decoding
bytes, testing crypto hypotheses, transforming constants, or validating a small
Python script against extracted input.

The feature is intentionally not designed as a one-off `run_python_snippet`
helper. Local execution is a high-risk agent capability. Wuwe should model it as
part of a reusable capability platform with explicit permission policy, user
approval, audit records, and replaceable sandbox backends.

## Current Implementation Status

Implemented in the first baseline:

- Public contracts for `agent/capability`, `agent/approval`, `agent/audit`,
  `agent/sandbox`, and `agent/execution`.
- `execution_runtime` policy, approval, audit, and backend orchestration.
- `controlled_process_backend` for Windows Python snippets.
- No-shell process launch through `CreateProcessW`.
- Host-owned environment allowlist.
- Temporary script materialization in the execution workdir.
- stdin, stdout, and stderr pipe handling.
- Timeout and cancellation process-tree cleanup on Windows when Job Objects are
  enabled.
- Windows Job Object kill-on-close behavior.
- Windows Job Object process count, CPU time, and memory limits.
- stdout and stderr byte limits with truncation flags.
- code, stdin, and total input byte limits enforced before backend launch.
- Backend enforcement contract metadata for process, resource, file, and
  network capabilities.
- Backend availability metadata for planned or unavailable backends.
- Default execution backend registry with `controlled_process`,
  `restricted_process`, `container`, and `wasm` slots.
- Registry selection by enforced backend requirements.
- Host-side path boundary helpers for canonical root checks.
- `execution_tool_provider` exposing `run_python_snippet` or a host-selected
  tool name such as `run_analysis_script`.
- Focused `execution_tests`, including policy/runtime/tool behavior and Python
  integration when a Python interpreter is available at configure time.

Still future work:

- Strong OS sandboxing beyond `controlled_process`.
- Cross-platform process backend implementations for Linux and macOS.
- Strong path-root enforcement inside executed code.
- Container and WASM backends.
- ReArk-side UI approval and audit persistence integration.

See [Controlled Local Execution Stage Record](execution-platform-stage.md) for
the current completed/not-completed checklist, registry behavior, and backend
enforcement contract.

## Design Principles

- Local execution is a capability, not a reasoning mode.
- Reasoning and Planning should not know about Python, processes, or sandboxes.
- Host policy always wins over model-supplied arguments.
- Model-visible tools expose the smallest useful argument surface.
- Permissions are never accepted directly from the model.
- Shell execution is not part of the first release.
- Audit records are a first-class output of execution, not best-effort logs.
- Isolation is represented explicitly so Wuwe does not overstate safety.
- Backends are replaceable without changing the agent tool schema.
- The first implementation must be useful for ReArk while leaving a clean path
  to stronger process, container, or WASM isolation.

## Agentic Pattern Fit

Controlled execution composes several agentic design patterns:

- Tool use: execution is exposed to ReAct or plan execution as an ordinary Wuwe
  tool provider.
- Planning: multi-step analysis can plan "extract input", "run script", and
  "interpret output" without embedding process details in Planning.
- Reflection: generated code or execution results can be reviewed before a
  retry, revision, or block decision.
- Human-in-the-loop: high-risk capability escalation can request host approval
  before execution.
- Guardrails and monitoring: policy checks, bounded resources, trace records,
  and audit events make execution inspectable and testable.

## Design Rationale And Source Mapping

This design records the architecture decisions discussed for ReArk and maps
them to the agentic patterns and coding-agent practices that motivated the
module boundaries.

The local design input was Antonio Gulli's *Agentic Design Patterns: A
Hands-On Guide to Building Intelligent Systems*. The relevant patterns are:

- Tool Use: execution is a tool invoked by an orchestration layer, not an LLM
  capability and not a Reasoning mode.
- Planning: multi-step analysis should plan and execute through named tools
  without embedding process-management details in plan logic.
- Reflection: generated scripts and execution results can be reviewed before a
  retry, revision, or block decision.
- Human-in-the-loop: high-risk execution requests require a host-mediated
  approval path instead of silent privilege escalation.
- Guardrails/Safety Patterns: tool restrictions, bounded resources, validation,
  and policy checks are part of the architecture, not prompt-only behavior.
- Evaluation and Monitoring: execution needs structured trace and audit records
  so hosts can debug, review, and evaluate agent trajectories.
- Context Engineering: inputs and outputs must be selected, bounded, and
  packaged deliberately so execution results help the model without flooding
  its context.
- MCP and external-tool patterns: tool access should preserve explicit trust
  boundaries and least-privilege integration.

The external practice mapping is:

- OpenAI Agents SDK separates agents, tools, guardrails, tracing, human
  intervention, MCP integration, and sandbox-agent concepts. Wuwe mirrors that
  separation by keeping execution outside Reasoning and by making audit,
  approval, and sandboxing explicit extension points.
  Reference: <https://openai.github.io/openai-agents-python/>
- Claude Code models permissions and sandboxing as policy-controlled runtime
  configuration with allow, ask, and deny rules plus filesystem and network
  sandbox settings. Wuwe mirrors this with host-owned execution policy,
  capability requests, approval decisions, and explicit isolation levels.
  Reference: <https://code.claude.com/docs/en/settings>
- MCP security guidance for local tool execution emphasizes explicit user
  consent, showing commands before execution, minimal privileges, sandboxing,
  and audit clarity. Wuwe mirrors this by keeping shell disabled in Phase 1,
  exposing a narrow Python tool, and requiring approval/audit contracts for
  higher-risk capabilities.
  Reference:
  <https://modelcontextprotocol.io/docs/tutorials/security/security_best_practices>

## Goals

- Provide a generic execution contract for local code snippets and future
  command-like capabilities.
- Support Python snippets first.
- Capture stdout, stderr, exit status, timeout, cancellation, elapsed time, and
  truncation state.
- Enforce host-configured limits before a backend starts work.
- Avoid shell invocation in Phase 1.
- Avoid inheriting the host environment except for an explicit allowlist.
- Provide a clear approval hook for higher-risk policy decisions.
- Emit structured audit events for every attempted execution.
- Expose execution through a Wuwe tool provider that can be composed with
  product tools, memory tools, and knowledge tools.
- Keep the design portable enough for Windows, Linux, and macOS backends.
- Keep execution inputs and outputs deliberately bounded so host applications
  can control how much code, stdin, stdout, and stderr enter the model context.

## Non-Goals

Phase 1 does not provide:

- An interactive shell.
- Arbitrary command execution.
- Long-running background jobs.
- Default network access.
- Default file write access.
- Full host environment inheritance.
- Model-controlled permission escalation.
- A claim that an ordinary local Python process is a strong security sandbox.
- A guarantee that network or arbitrary file reads are impossible without an OS
  sandbox backend.

## Threat Model

The execution runtime treats model-generated code as untrusted input.

Phase 1 is expected to defend against:

- The model bypassing Wuwe tools by requesting a raw shell command.
- Unbounded execution time.
- Unbounded stdout or stderr growth.
- Accidental leakage of unrestricted environment variables.
- Model/tool arguments silently enabling shell, network, file-write, or
  environment access.
- Accidental host-side file writes performed by Wuwe itself before policy
  approval.
- Tool arguments that attempt to set their own permissions.
- Silent high-risk actions without host visibility.
- Unobservable execution failures.

Phase 1 is not expected to fully defend against:

- A Python interpreter reading files that are reachable by the current OS user
  when no OS-level filesystem sandbox is active.
- A Python interpreter opening sockets when no OS-level network sandbox is
  active.
- Native extension modules escaping Python-level restrictions.
- Platform kernel or container vulnerabilities.
- Malicious code that abuses allowed roots or allowed network destinations.

The documentation, API names, and result metadata must preserve this boundary.
The first backend is a controlled process backend. Strong sandboxing is a
backend capability added later through the Sandbox module.

In Phase 1, `network: false` and `file_write: false` are host policy and tool
surface guarantees, not kernel-enforced restrictions for arbitrary Python code.
Those settings prevent the model from requesting extra permissions through the
tool call and keep high-risk capabilities out of approval flow unless the host
opts in. They do not prevent Python from using OS privileges already available
to the current process. A backend must advertise `network_restriction`,
`filesystem_read_restriction`, or `filesystem_write_restriction` before product
documentation can claim OS-enforced isolation.

## Module Set

Controlled execution introduces five long-term modules:

| Module | Responsibility |
| --- | --- |
| `agent/capability` | Common capability names, requested operations, and policy evaluation vocabulary. |
| `agent/approval` | Host-mediated confirmation requests, decisions, scopes, and denial reasons. |
| `agent/audit` | Shared audit event contracts and sinks for high-risk agent actions. |
| `agent/sandbox` | Isolation-level abstraction and backend capability discovery. |
| `agent/execution` | Execution requests, policies, runtime orchestration, backends, and tool providers. |

The first implementation may keep the code physically under
`agent/execution` if needed for delivery speed, but the public types should be
named and shaped so they can move into standalone modules without API churn.

## Layering

```text
reasoning_runner / plan_runner
        |
compose_tool_providers(...)
        |
execution_tool_provider
        |
execution_runtime
        |
capability_policy
        |
approval_service
        |
audit_sink
        |
sandbox_backend
        |
controlled Python process
```

Reasoning and Planning only see a normal tool provider. The execution runtime
owns request normalization, policy checks, approval, backend dispatch, timeout,
cancellation, output capture, and audit emission.

## Public Header Plan

```text
include/wuwe/agent/capability/
  capability.hpp
  capability_policy.hpp

include/wuwe/agent/approval/
  approval.hpp
  approval_service.hpp

include/wuwe/agent/audit/
  audit.hpp
  audit_sink.hpp

include/wuwe/agent/sandbox/
  sandbox.hpp
  sandbox_backend.hpp

include/wuwe/agent/execution/
  execution_core.hpp
  execution_policy.hpp
  execution_runtime.hpp
  execution_backend.hpp
  execution_tools.hpp
  execution_codec.hpp
  execution.hpp
```

The module umbrella `<wuwe/agent/execution/execution.hpp>` includes the
execution contract and tool provider. The top-level `<wuwe/wuwe.h>` may include
it after the first implementation is stable.

## Capability Model

Capabilities describe what an agent action wants to do. They are stable policy
keys, not implementation details.

Initial capability names:

```text
process.python
process.shell
filesystem.read
filesystem.write
network.outbound
environment.read
secret.read
```

Phase 1 uses `process.python` and may derive `filesystem.read`,
`filesystem.write`, and `network.outbound` from the host policy, but it does not
grant the model control over those capabilities.

Capability requests should include:

- capability name,
- human-readable summary,
- risk level,
- requested resource paths or endpoints when applicable,
- model-visible tool name,
- trace id or execution id,
- metadata for host UI and audit records.

## Approval Model

Approval is required when a request exceeds the automatic policy. The approval
module should make the decision explicit and auditable.

Planned types:

```cpp
enum class approval_decision_kind {
  approved,
  denied,
  needs_manual_review,
};

enum class approval_scope {
  once,
  session,
  workspace,
};

struct approval_request {
  std::string id;
  std::string summary;
  std::vector<capability_request> capabilities;
  std::map<std::string, std::string> metadata;
};

struct approval_decision {
  approval_decision_kind kind;
  approval_scope scope;
  std::string reason;
  std::map<std::string, std::string> metadata;
};
```

The approval service is host-provided. GUI hosts can display a confirmation
dialog. CLI hosts can prompt or deny. Services can route the request to a policy
engine. Tests can use deterministic allow or deny implementations.

## Audit Model

Every attempted execution emits an audit event, including denied and failed
attempts.

Audit records should include:

- execution id,
- parent trace id when available,
- tool name,
- language,
- isolation level,
- capability requests,
- policy decision,
- approval decision when used,
- code hash and bounded code preview,
- stdin size and bounded preview,
- workdir,
- readable and writable root summaries,
- environment variable names supplied, not values,
- timeout and output limits,
- start time, elapsed time, and termination reason,
- exit code when available,
- stdout and stderr sizes,
- stdout and stderr truncation flags,
- bounded stdout and stderr previews,
- backend name and backend error details.

Audit sinks must not force applications to persist sensitive data. The default
event should contain bounded previews and stable hashes so hosts can choose
their own retention policy.

## Sandbox Model

Isolation is represented explicitly:

```cpp
enum class isolation_level {
  none,
  controlled_process,
  restricted_process,
  container,
  wasm,
};
```

`controlled_process` means Wuwe controls interpreter selection, environment,
working directory, timeout, cancellation, stdout, stderr, and process lifecycle.
It does not imply OS-level filesystem or network isolation.

`restricted_process` is reserved for OS-specific restrictions such as Windows
Job Objects, restricted tokens, AppContainer-style execution, seccomp, pledge,
or sandbox-exec-like mechanisms.

`container` is reserved for Docker, containerd, podman, or product-managed
containers.

`wasm` is reserved for WASI-style execution with a stronger capability-based
resource model.

Backends must report their supported isolation level and supported features.
The runtime must not silently label a backend as stronger than it is.

## Execution Core Contract

Model-visible tools should not expose the full execution request. The full
request is an internal host and runtime contract.

Planned low-level request:

```cpp
enum class execution_language {
  python,
};

struct execution_limits {
  std::chrono::milliseconds timeout { 3000 };
  std::size_t max_stdout_bytes { 65536 };
  std::size_t max_stderr_bytes { 65536 };
  std::size_t max_code_bytes { 65536 };
  std::size_t max_stdin_bytes { 1048576 };
  std::size_t max_total_input_bytes { 1114112 };
};

struct execution_request {
  execution_language language { execution_language::python };
  std::string code;
  std::string stdin_text;
  std::filesystem::path workdir;
  execution_limits limits;
  std::map<std::string, std::string> env;
  std::map<std::string, std::string> metadata;
};
```

Planned result:

```cpp
enum class execution_termination_reason {
  exited,
  timeout,
  cancelled,
  launch_failed,
  policy_denied,
  approval_denied,
  backend_error,
};

struct execution_result {
  std::optional<int> exit_code;
  execution_termination_reason termination_reason {
    execution_termination_reason::backend_error
  };
  bool timed_out { false };
  bool cancelled { false };
  bool stdout_truncated { false };
  bool stderr_truncated { false };
  std::string stdout_text;
  std::string stderr_text;
  std::string error_message;
  std::chrono::milliseconds elapsed { 0 };
  std::map<std::string, std::string> metadata;
};
```

`exit_code` is optional because timeout, cancellation, launch failure, and
policy denial may not produce a meaningful process exit code.

## Execution Policy

Policy belongs to the host and runtime, not the model.

Planned policy fields:

```cpp
struct execution_policy {
  std::vector<execution_language> allowed_languages;
  std::filesystem::path default_workdir;
  std::vector<std::filesystem::path> readable_roots;
  std::vector<std::filesystem::path> writable_roots;
  execution_limits max_limits;
  bool allow_network { false };
  bool allow_file_read { false };
  bool allow_file_write { false };
  bool allow_shell { false };
  bool require_approval_for_network { true };
  bool require_approval_for_file_write { true };
  bool require_approval_for_shell { true };
  std::map<std::string, std::string> allowed_env;
};
```

Policy evaluation must:

- reject unsupported languages,
- clamp requested limits to policy maxima,
- reject code, stdin, or total input that exceeds configured input limits
  before backend launch,
- reject shell execution in Phase 1,
- use a host-selected working directory when the request does not provide one,
- canonicalize paths before checking roots,
- avoid inheriting unspecified environment variables,
- compute requested capabilities,
- call approval when the policy requires it,
- return a structured denial result instead of throwing for ordinary policy
  denial.

Path checks must handle relative paths, `..`, symlinks, junctions, and platform
case-sensitivity. On Windows, canonicalization must account for drive letters
and case-insensitive comparison where appropriate.

## Execution Runtime

`execution_runtime` is the main API for host applications.

Responsibilities:

- create a stable execution id,
- normalize and validate the request,
- evaluate policy,
- request approval when needed,
- emit audit start and finish events,
- dispatch to the configured backend,
- enforce timeout and cancellation,
- normalize backend failures into `execution_result`,
- return bounded stdout and stderr.

Sketch:

```cpp
class execution_backend {
public:
  virtual ~execution_backend() = default;

  [[nodiscard]] virtual sandbox_backend_info info() const = 0;

  [[nodiscard]] virtual execution_result run(
    const execution_request& request,
    std::stop_token stop_token) = 0;
};

class execution_runtime {
public:
  execution_runtime(
    std::unique_ptr<execution_backend> backend,
    execution_policy policy,
    audit_sink* audit,
    approval_service* approvals);

  [[nodiscard]] execution_result run(
    execution_request request,
    std::stop_token stop_token = {});
};
```

The runtime owns the backend through `std::unique_ptr` because there is one
clear owner. Audit and approval services are non-owning optional collaborators.
If shared ownership is needed by a host, the host can wrap them externally.

## Tool Provider

The first agent-facing tool should be intentionally narrow:

```text
run_python_snippet(code, stdin_text?, timeout_ms?)
```

The model can request code, optional stdin, and a timeout hint. The runtime
clamps or rejects the timeout based on host policy. The model cannot request
network, file write, environment variables, interpreter path, command-line
arguments, or shell.

For ReArk, the host can expose a product-specific name:

```text
run_analysis_script(code, stdin_text?, timeout_ms?)
```

The tool provider maps that model-facing call into an `execution_request` and
uses the host policy:

```text
language: python
isolation: controlled_process
network: false
file_write: false
timeout: 3 to 5 seconds
workdir: ReArk analysis temp directory
stdin: selected bytes, constants, decompiled text, or resource data
```

For `controlled_process`, `network: false` and `file_write: false` mean those
capabilities are not exposed to the model or approval flow. They are not
OS-enforced Python restrictions. ReArk should use a host-selected temporary
working directory, pass selected data through stdin, and avoid placing
sensitive files in locations reachable by the execution user until a restricted
backend is available.

The tool result returned to the model should contain:

- termination reason,
- exit code when available,
- timed out flag,
- cancelled flag,
- stdout text,
- stderr text,
- truncation flags,
- concise diagnostic message.

The tool provider should also enforce context packaging rules:

- stdin is supplied by the host or model as task-local input, not by granting
  broad filesystem access;
- large binary inputs should be passed as host-selected hex, base64, or
  temporary file handles only when policy allows that form;
- stdout and stderr returned to the model are bounded and marked when
  truncated;
- tool results should include enough metadata for the model to reason about
  failure without exposing unnecessary host state.

## Phase 1 Delivery Scope

Phase 1 should implement:

- `agent/execution` public contract.
- Controlled Python process backend.
- Fixed interpreter configured by host or discovered through a safe default.
- No shell invocation.
- code, stdin, and total input byte limits.
- Temporary workdir support.
- Environment allowlist.
- Timeout.
- Cooperative cancellation through `std::stop_token`.
- Process-tree cleanup on timeout or cancellation on Windows when Job Objects
  are enabled.
- Job Object kill-on-close on Windows.
- Job Object process count, CPU time, and memory limits on Windows.
- Backend enforcement contract metadata.
- Backend availability metadata.
- Default backend registry with planned restricted/container/WASM slots.
- Requirement-based backend selection that skips unavailable or under-enforced
  backends.
- Host-side canonical path/root boundary helpers.
- stdout and stderr capture with byte limits.
- stdout and stderr truncation flags.
- Structured policy denial results.
- Structured audit events.
- `execution_tool_provider` exposing `run_python_snippet`.
- Provider-level raw argument byte limits before JSON parsing.
- Policy-aligned tool schemas with `maxLength`, timeout bounds, and
  `additionalProperties=false` by default.
- Audit events for provider-level argument rejection paths.
- Tests for policy, timeout, cancellation, truncation, launch failure, and audit.

Phase 1 may keep `agent/capability`, `agent/approval`, `agent/audit`, and
`agent/sandbox` as minimal public contracts if full standalone implementations
would delay the usable execution path.

## Future Phases

### Phase 2: Stronger Local Restrictions

- Replace the planned `restricted_process` placeholder with an available
  backend only when the configured backend can enforce its advertised contract.
- Windows restricted backend using restricted tokens or AppContainer-style
  execution.
- OS-enforced filesystem read/write restrictions.
- OS-enforced network denial.
- Stronger workdir and root restrictions enforced inside executed code.
- Optional explicit file materialization from host-provided inputs.

### Phase 3: Container Backend

- Replace the planned `container` placeholder with an available backend only
  when the configured container runtime can enforce its advertised contract.
- Docker or containerd backend.
- Network namespace control.
- Read-only mounts.
- Explicit writable scratch mount.
- Image allowlist.
- Per-run cleanup and audit.

### Phase 4: WASM Backend

- Replace the planned `wasm` placeholder with an available backend only when a
  WASI runtime is integrated and policy limits are enforced.
- WASI execution for small deterministic tools.
- Capability-based filesystem preopens.
- Stronger default no-network behavior.
- Better portability for cloud and desktop hosts.

### Phase 5: Additional Tool Surfaces

- JavaScript snippets.
- Domain-specific reverse-engineering helpers.
- MCP exposure for hosts that want to offer controlled execution remotely.
- Policy-driven shell command execution, still disabled by default.

## ReArk Integration

ReArk should not embed a separate script runner. It should use Wuwe's execution
runtime and expose a product-specific tool provider.

Expected integration:

```cpp
namespace execution = wuwe::agent::execution;

auto runtime = std::make_shared<execution::execution_runtime>(
  execution::make_controlled_process_backend(...),
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
    },
    .allow_network = false,
    .allow_file_read = false,
    .allow_file_write = false,
    .allow_shell = false,
    .allowed_env = {},
  },
  audit_sink,
  approval_service);

auto execution_tools =
  std::make_shared<execution::execution_tool_provider>(
    *runtime,
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

The provider schema is generated from the host policy and tool options. It does
not expose permission controls, environment variables, shell flags, interpreter
paths, or backend selection. Unknown arguments are rejected by default and
audited as `schema_invalid`. Raw argument JSON that exceeds
`max_arguments_bytes` is rejected before parsing and audited as
`arguments_limit`. Timeout hints outside the configured bounds are rejected and
audited as `timeout_limit` unless the host explicitly opts into runtime clamp
semantics.

ReArk owns:

- whether the tool is enabled,
- default policy,
- workdir selection,
- selected input materialization,
- user approval UI,
- result rendering,
- audit persistence policy.

Wuwe owns:

- execution request and result contracts,
- policy evaluation,
- approval request shape,
- process lifecycle,
- timeout and cancellation,
- stdout and stderr capture,
- audit event shape,
- tool provider integration.

## Reasoning And Planning Integration

ReAct-style reasoning should use the execution tool as a normal tool call. A
model may generate a Python snippet, run it, observe stdout or stderr, revise,
and answer.

Plan execution should use `tool_plan_executor` or a future typed adapter. A
plan can contain explicit steps such as:

```text
1. Extract the encrypted byte array.
2. Write a short Python decoder.
3. Run the decoder with the extracted bytes on stdin.
4. Interpret stdout and verify the result.
```

Planning still does not depend on Python. It only invokes a named tool through
the existing tool-provider contract.

## Validation Matrix

Minimum tests:

| Area | Required cases |
| --- | --- |
| Policy | unsupported language, shell denied, timeout clamped, env not inherited |
| Input limits | code over limit, stdin over limit, total input over limit, exact boundary allowed |
| Paths | relative workdir, parent traversal, symlink or junction behavior, root comparison |
| Execution | successful Python snippet, nonzero exit, stderr-only output |
| Timeout | infinite loop terminates, result has `timed_out=true` |
| Cancellation | `std::stop_token` terminates process, result has `cancelled=true` |
| Output | stdout truncation, stderr truncation, exact limit boundary |
| Launch | missing interpreter, invalid workdir, backend failure |
| Audit | attempted, denied, approved, started, completed, failed, timed out |
| Tool | JSON argument parsing, raw argument byte limit, unknown field rejection, timeout hint rejection/clamp, unknown tool name |
| Integration | composed provider order, Reasoning ReAct tool call, Planning tool step |

Windows-specific tests should verify process-tree cleanup when child processes
are introduced by future backends. Phase 1 should at least ensure the direct
Python process is terminated.

## Observability

Execution events should be bridgeable into Wuwe's shared observability contract
and Reasoning trace records. The event stream should not expose hidden
chain-of-thought. It should expose observable lifecycle events:

- policy evaluation started,
- policy denied,
- approval requested,
- approval denied,
- execution started,
- execution output truncated,
- execution completed,
- execution timed out,
- execution cancelled,
- execution failed.

## Security Review Checklist

Before enabling execution by default in any host:

- Is the tool opt-in?
- Is shell disabled?
- Is the interpreter path host-controlled?
- Are environment variables allowlisted?
- Are stdout and stderr bounded?
- Is timeout nonzero?
- Does cancellation terminate the process?
- Are policy denials returned as structured results?
- Are audit events emitted for denied and failed attempts?
- Does the UI show high-risk capability requests before approval?
- Does product documentation avoid claiming strong sandboxing unless the active
  backend actually provides it?

## Open Questions

- Which Python interpreter discovery policy should Wuwe provide by default:
  host-required path, package-bundled interpreter, PATH lookup, or explicit
  resolver callback?
- Should Phase 1 materialize snippets as temporary files or pass code through
  stdin with `python -`?
- Should tool output returned to the model include stderr by default, or should
  hosts be able to redact stderr separately?
- Should audit code previews be disabled by default for privacy-sensitive
  hosts, leaving only hashes and sizes?
- Which sandbox backend should be prioritized after controlled process:
  Windows restricted process, container, or WASM?

## Decision

Wuwe will add controlled local execution as a capability-platform feature, not
as a ReArk-only helper. The first deliverable is a controlled Python snippet
runtime with no shell, bounded resources, cancellation, audit events, and a
tool provider. The architecture reserves stable extension points for capability
policy, approval, audit, and stronger sandbox backends so future high-risk tools
do not need a parallel security model.
