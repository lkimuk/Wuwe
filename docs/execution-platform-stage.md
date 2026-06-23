# Controlled Local Execution Stage Record

Status: P0/P1 complete and pushed; P2/P3 platform contract and backend
selection surfaces are implemented; P2/P3 strong OS/container/WASM execution
backends remain future work.

Date: 2026-06-22

## Boundary Statement

This stage hardens Wuwe's controlled local execution platform. It does not
turn `controlled_process` into a strong filesystem or network sandbox.

`controlled_process` means Wuwe executes a host-selected interpreter and owns
interpreter validation/diagnostics, environment allowlisting, workdir selection,
stdin/stdout/stderr handling, timeout, cancellation, process-tree cleanup on
Windows, resource limits where Windows Job Objects can enforce them, structured
result JSON, and audit metadata. Product-level Python discovery and selection
remain host responsibilities.

Strong filesystem and network isolation must come from a backend that explicitly
advertises that contract, such as a future restricted-process, container, or
WASM backend.

## Completed In P0

- `execution_tool_options` supports raw argument byte limits, empty-stdin
  policy, unknown-argument policy, and timeout min/max/default behavior.
- Tool providers reject oversized raw arguments before JSON parsing.
- Tool schemas expose only `code`, `stdin_text`, and `timeout_ms`.
- Tool schemas publish code/stdin max lengths and timeout bounds from host
  policy and tool options.
- Unknown arguments are rejected by default.
- Timeout hints outside configured bounds can be rejected before backend launch.
- Provider rejections return structured execution JSON with
  `termination_reason=policy_denied`.
- Provider rejections are audited as `arguments_limit`, `schema_invalid`, or
  `timeout_limit`.
- Runtime policy denials and limit clamps are reflected in result metadata and
  audit metadata.
- Execution-finished audit records include backend name, isolation level, and
  enforcement status for file/network denial.

## Completed In P1

- Windows `controlled_process_backend` can create a Job Object per execution.
- Job Objects are configured with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`.
- Timeout and cancellation terminate the whole Job Object when enabled, not only
  the direct Python process.
- Process count limits are represented in `execution_limits` and enforced by
  Windows Job Objects.
- Job memory limits are represented in `execution_limits` and enforced by
  Windows Job Objects.
- Job CPU time limits are represented in `execution_limits` and enforced by
  Windows Job Objects.
- `controlled_process_backend_config::use_job_object` allows hosts to disable
  Job Object behavior when required.
- Backend enforcement metadata now distinguishes enforced, not enforced,
  partial, planned, and not applicable capabilities.
- `controlled_process` advertises process-tree cleanup and resource enforcement
  only when the active platform/configuration can enforce it.
- `probe_python_interpreter(...)` validates host-selected Python interpreters
  with no-shell launch, timeout, stdout/stderr capture, version/executable
  reporting, and structured failure status.
- `controlled_process_backend` includes stable Python launch diagnostic
  metadata such as `error_code`, `launch_error_code`, `launch_error_message`,
  `python_interpreter`, and `timeout_phase`.
- Audit events include process count, memory, CPU time, and enforcement fields.
- Execution results include Job Object/resource enforcement metadata.
- `execution_backend_registry` provides a default registry with
  `controlled_process`.
- `path_policy` provides canonical root-boundary helpers for host-side path
  validation and future restricted backends.
- Tests cover default backend registry behavior, disabled Job Object contracts,
  resource limit clamping/audit metadata, path prefix traps, parent traversal,
  child-process cleanup on timeout, AppContainer file boundaries, and a
  host-reachable loopback network-blocking probe.
- A private Windows AppContainer probe can launch a temporary minimal Python
  runtime copied into the AppContainer profile storage path without mutating the
  host Python installation; it verifies stdin/stdout/stderr handling,
  explicit environment allowlisting, stdout/stderr byte-limit truncation,
  allowed-write/denied-read behavior, parent-traversal and hardlink escape
  denial, Job active-process limits, and Job-backed timeout for the real
  interpreter, plus explicit cancellation through the same Job termination path.
- The minimal Python runtime staging path has been factored from the private
  probe into a library-internal restricted-process component with structured
  staging status, copied-file reporting, and system error capture. The
  AppContainer Python probe now validates this shared implementation.
- AppContainer profile creation, replacement, SID lifetime, and profile storage
  path resolution have also been factored into a library-internal Windows
  component and are exercised by the existing AppContainer probes.
- AppContainer no-shell process launch, explicit inherited stdio handle list,
  Job Object assignment, stdout/stderr byte limits, timeout, cancellation, and
  environment allowlist handling have been factored into a library-internal
  Windows launch component and are exercised by the AppContainer probes.
- AppContainer SID ACL grants for individual files, directories, and recursive
  trees have been factored into a library-internal Windows component and are
  exercised by the file-boundary and real-Python probes.
- Request-scoped workspace/script creation and cleanup have been factored into
  a library-internal component with structured lifecycle failures.
- A library-internal Windows restricted execution plan now composes the profile,
  minimal Python runtime staging, request workspace, ACL grants, environment
  allowlist, and AppContainer launch request into one tested execution path.
  A library-internal runner maps that path into `execution_result` metadata for
  successful and timeout executions. This is still not exposed as an executable
  backend factory.

## Completed In P2/P3 Platform Contract

- Backend metadata now exposes `available` and `unavailable_reason`.
- `restricted_process_backend_descriptor()` and
  `restricted_process_backend_config` provide a stable public contract surface
  for the future backend without exposing an executable backend factory.
- The restricted-process config defaults to request-scoped minimal Python
  runtime staging, no parent environment inheritance, network denial, Job Object
  lifecycle, and runtime staging cleanup.
- The default registry exposes four backend slots:
  - `controlled_process`: available.
  - `restricted_process`: planned and unavailable.
  - `container`: planned and unavailable.
  - `wasm`: planned and unavailable.
- Planned backend slots are descriptors only; they are not executable backend
  factories.
- `create("restricted_process")`, `create("container")`, and `create("wasm")`
  return `nullptr` until real implementations are registered.
- Planned backend contracts mark future enforcement as `planned`, not
  `enforced`.
- Registry selection can require specific enforced capabilities such as process
  tree cleanup, CPU/memory limits, filesystem read/write deny, or network deny.
- Registry selection skips unavailable backends.
- Registry selection returns no backend when strong filesystem/network isolation
  is required in the current build.
- Package smoke now verifies the backend registry, selection API, and Python
  interpreter diagnostics API are visible from the installed package.

## Not Completed

- `controlled_process` still does not enforce file read denial inside Python.
- `controlled_process` still does not enforce file write denial inside Python.
- `controlled_process` still does not enforce network denial inside Python.
- Windows restricted token/AppContainer backend is not implemented.
- Windows restricted backend acceptance criteria and implementation sequence
  are recorded in
  [Restricted Execution Backend Plan](execution-restricted-backend-plan.md).
- Container backend is not implemented.
- WASM/WASI backend is not implemented.
- Cross-platform Linux/macOS controlled-process resource governance is not
  implemented.
- ReArk UI approval and audit persistence are host-side work and are not part of
  this library stage.

## Backend Enforcement Contract

Current `controlled_process` contract on Windows with Job Objects enabled:

- shell execution: enforced by API/backend design.
- timeout: enforced.
- cancellation: enforced.
- stdout/stderr byte limits: enforced in Wuwe pipe readers.
- environment allowlist: enforced by Wuwe-provided environment block.
- working directory: enforced for process launch location.
- process tree cleanup: enforced through Job Object termination.
- process count limit: enforced through Job Object limits.
- CPU time limit: enforced through Job Object limits.
- memory limit: enforced through Job Object limits.
- filesystem read deny: not enforced.
- filesystem write deny: not enforced.
- network deny: not enforced.

If Job Objects are disabled, process-tree cleanup, process count, CPU time, and
memory limits are reported as `not_enforced`.

## ReArk Guidance

ReArk should use `execution_tool_options` and Wuwe's policy/runtime validation
for generic tool boundaries, while keeping product policy in ReArk:

- whether execution is enabled,
- Python interpreter discovery, selection, and packaging,
- per-run workdir lifecycle,
- user approval UX,
- audit persistence,
- selected package/resource input preparation,
- UI rendering of timeout, cancellation, truncation, stdout, and stderr.

ReArk must not present `controlled_process` as a secure filesystem or network
sandbox. UI and documentation should show backend enforcement metadata directly.

## Verification

Focused verification for this stage:

```text
cmake --build D:\MilesLi\Wuwe\build --config Debug --target execution_tests
ctest --test-dir D:\MilesLi\Wuwe\build -C Debug -R execution_tests --output-on-failure
```

The final stage verification should also include the full Debug test suite,
Release build, and install smoke before commit/push.
