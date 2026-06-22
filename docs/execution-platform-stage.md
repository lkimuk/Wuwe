# Controlled Local Execution Stage Record

Status: P0 complete and pushed; P1 controlled-process hardening implemented
locally for this stage; P2/P3 stronger sandbox and multi-backend work remains
future work.

Date: 2026-06-22

## Boundary Statement

This stage hardens Wuwe's controlled local execution platform. It does not
turn `controlled_process` into a strong filesystem or network sandbox.

`controlled_process` means Wuwe owns interpreter selection, environment
allowlisting, workdir selection, stdin/stdout/stderr handling, timeout,
cancellation, process-tree cleanup on Windows, resource limits where Windows
Job Objects can enforce them, structured result JSON, and audit metadata.

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
- Audit events include process count, memory, CPU time, and enforcement fields.
- Execution results include Job Object/resource enforcement metadata.
- `execution_backend_registry` provides a default registry with
  `controlled_process`.
- `path_policy` provides canonical root-boundary helpers for host-side path
  validation and future restricted backends.
- Tests cover default backend registry behavior, disabled Job Object contracts,
  resource limit clamping/audit metadata, path prefix traps, parent traversal,
  and child-process cleanup on timeout.

## Not Completed

- `controlled_process` still does not enforce file read denial inside Python.
- `controlled_process` still does not enforce file write denial inside Python.
- `controlled_process` still does not enforce network denial inside Python.
- Windows restricted token/AppContainer backend is not implemented.
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
- Python interpreter selection,
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
