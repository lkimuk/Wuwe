# Restricted Execution Backend Plan

Status: design and acceptance record for the P2 backend. The default registry
still keeps `restricted_process` unavailable, while Windows builds now expose an
explicit opt-in restricted backend factory and registry path.

Date: 2026-06-23

## Boundary

`restricted_process` must not be a wrapper around `controlled_process`.

It may become available only when the backend can prove the enforcement it
advertises. Until then, the default registry should expose it as an unavailable
descriptor with planned enforcement fields, and `create("restricted_process")`
must return `nullptr`.

The public `restricted_process_backend_descriptor()` and
`restricted_process_backend_config` types now provide a stable contract surface
for the backend. `make_restricted_process_backend(...)` exposes a real factory
only when the configured contract is available. The default registry still does
not make `restricted_process` available; hosts must opt in through
`make_execution_backend_registry(...)` options.

The config already encodes the intended production defaults: host-selected
Python interpreter, request/workdir fallback, readable and writable roots,
explicit base environment, no parent environment inheritance, no network,
Windows Job Object lifecycle, request-scoped minimal Python runtime staging, and
runtime staging cleanup. These defaults mirror the current AppContainer probes
without claiming that the backend is executable yet.

## Windows Candidate

The Windows implementation should be based on OS-enforced process identity and
resource controls:

- AppContainer or a restricted token for the child process identity.
- Job Objects for lifecycle and resource limits.
- Explicit per-run workdir and optional readable roots.
- ACL grants for only the files and directories the child identity may access.
- No network capabilities for the AppContainer profile when network denial is
  required.
- No shell execution.
- Inherited handle list limited to stdio handles.

The Windows SDK on the current development machine includes the APIs needed for
an AppContainer probe:

- `CreateAppContainerProfile`
- `DeriveAppContainerSidFromAppContainerName`
- `DeleteAppContainerProfile`
- `SECURITY_CAPABILITIES`
- `PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES`
- `UserEnv.lib`

This makes a real probe feasible, but it does not by itself prove that Python
can run safely inside the restricted identity. Python may need read/execute
access to its interpreter, standard library, DLLs, and per-run script files.
Those grants must be explicit and test-covered.

An initial AppContainer launch probe now exists. It creates a temporary
AppContainer profile, grants the AppContainer SID read/execute access to a
test-only child executable, launches the child with
`PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES`, captures stdio through an
explicit inherited handle list, assigns the child to a Job Object, and verifies
that the child reports `TokenIsAppContainer`.

The same probe starts a host-reachable loopback TCP listener, verifies that the
host can connect to it, then launches the no-capability AppContainer child
against that listener. The child reports an initial pending connect
(`WSAEWOULDBLOCK`) followed by timeout (`WSAETIMEDOUT`) instead of a successful
connection. The probe treats this as network-blocked evidence for the
AppContainer identity without requiring the OS to surface `WSAEACCES`.

A follow-up AppContainer file-boundary probe runs the copied test executable
inside the AppContainer profile storage path and performs file operations from
inside the child process. It proves allowed file read, allowed directory write,
denied file read, denied directory write, and parent traversal denial for that
AppContainer identity. This is stronger than host-side `AccessCheck`, but it is
still not enough to advertise a restricted backend because network denial and a
real Python/runtime launch path remain unaccepted.

## Minimum Acceptance Criteria

The backend may advertise `available=true` only after automated tests prove:

- Process launch succeeds under the restricted identity.
- The process still runs without a shell.
- The process is assigned to a Job Object with kill-on-close.
- Timeout and cancellation clean up the full process tree.
- Process count, CPU time, and memory limits remain enforced.
- The child can read only explicitly allowed readable roots.
- The child can write only explicitly allowed writable roots.
- Parent traversal, root-prefix traps, symlinks, and junctions do not escape the
  allowed roots.
- The child cannot read a denied file reachable by the host user.
- The child cannot write a denied file reachable by the host user.
- Network denial is verified with an outbound socket attempt.
- Audit/result metadata records the backend name, isolation level, and each
  enforcement field as `enforced`, `partial`, or `not_enforced`.
- If any required restriction is unavailable on the current platform or build,
  registry selection returns no backend for that requirement.

## Implementation Sequence

1. Add a private Windows AppContainer launch probe in tests, using a child mode
   of the test executable before exposing any public backend factory.
2. Prove identity, stdio capture, Job Object cleanup, and no-shell behavior.
3. Add explicit ACL grant helpers for per-run files and directories.
4. Add denied read/write and traversal tests.
5. Add network-deny verification.
6. Factor the proven launch path into `restricted_process_backend`.
7. Register the backend only on platforms/builds where its advertised contract
   passes the same checks, and only behind an explicit host opt-in.

## Current Probe Checkpoints

Private Windows probes now exist in `execution_tests`.

The launch probe starts the test executable in a child mode with a token
produced by `CreateRestrictedToken(DISABLE_MAX_PRIVILEGE)`, verifies
`TokenHasRestrictions`, captures stdout/stderr through inherited stdio handles,
uses an explicit inherited handle list, avoids shell execution, and assigns the
child to a Job Object with kill-on-close and an active process limit.

The ACL probe creates protected DACLs for allowed and denied read/write roots,
builds a restricted token with restricting SIDs, and uses Windows `AccessCheck`
against the real file security descriptors. It proves that the host user can
read/write the denied objects while the restricted token is allowed for the
allowed roots and denied for the denied roots.

This is still not a filesystem or network sandbox. A restricted-token child
with restricting SIDs still needs a proven launch path through Windows process
initialization objects, executable/DLL access, script files, workdir, and
allowed roots before any public backend is exposed. Do not expose
`restricted_process` from these probes alone.

The AppContainer probe proves a second launch path: a child can be created under
an AppContainer identity with stdio capture, an explicit inherited handle list,
and Job Object assignment. It also records immediate and final socket errors and
proves that the child cannot reach a loopback listener that the host can reach.
This is still a checkpoint, not a public backend acceptance pass, because the
final runtime launch contract still needs to run the real interpreter with the
same file, network, lifecycle, and resource controls.

The AppContainer file-boundary probe also proves child-process enforcement for
allowed read, allowed write, denied read, denied write, and `..` traversal
denial within the AppContainer profile storage root. Together with the
host-reachable loopback network-blocking probe, this advances the Windows
candidate materially.

A minimal Python runtime launch probe now copies the host-selected Python
interpreter, required adjacent runtime DLLs, and a minimal standard-library
subset into the AppContainer profile storage path. It grants the AppContainer
SID read/execute access to that temporary runtime, launches Python with no shell
through the same AppContainer stdio/Job Object helper, verifies that a tiny
script reads stdin and writes stdout without stderr, verifies explicit
environment allowlisting without leaking an unrelated parent-process variable,
proves that real Python can write an explicitly granted writable root while
failing to read a denied file, proves parent traversal back to the denied file
is blocked, proves a hardlink in the allowed tree cannot bypass the denied file
object's ACL, verifies stdout/stderr byte-limit truncation, proves Job
active-process limits prevent Python from spawning a child process, and verifies
that a hanging Python script is terminated by both the Job-backed timeout path
and an explicit cancellation request. This proves a real interpreter can start
and observe the same stdio, environment, file-boundary, process-count, and
lifecycle controls inside the restricted identity without mutating ACLs on the
host Python installation. The AppContainer profile lifecycle/storage-path
logic, AppContainer launch/stdio/Job Object helper, and minimal Python runtime
staging logic have now been factored into library-internal restricted-process
components. AppContainer SID ACL grants for files, directories, and recursive
trees, plus request-scoped workspace/script lifecycle handling, have also been
factored into internal components. A library-internal restricted execution plan
now composes these pieces into one real Python launch path exercised by tests.
An internal runner maps successful and timeout executions into
`execution_result` metadata.

The AppContainer launch request now receives the request's process-count,
memory, and CPU-time limits and applies them through the Job Object instead of
using a hard-coded active-process limit. Result metadata records those requested
limits and the active Job Object enforcement state.

An internal restricted backend candidate wraps this execution plan behind the
normal `execution_backend` interface for tests. It intentionally advertises
`available=false` and is not registered in the default backend registry. This
keeps an integration-shaped test surface for negative/default behavior. When run
through `execution_runtime`, the finished audit event receives the candidate's
result metadata under `result_*` keys, including plan status, launch status,
backend stage, candidate marker, and configured enforcement fields.

The public `restricted_process_backend_configured_contract(...)` helper exposes
the current configured enforcement diagnostics without changing default
registry behavior. On Windows, the helper reports no-shell launch, stdio,
environment, working directory, timeout, cancellation, and Job Object resource
controls as `enforced` when enabled. No-capability AppContainer outbound network
denial is `enforced`. Configured filesystem read/write root isolation is also
reported as `enforced`: a candidate test runs real Python and proves readable
roots can be read but not written, writable roots can update existing files and
create new files, and unlisted roots cannot be read or written.

The public `make_restricted_process_backend(...)` factory now uses the same
configured availability checks with a `registered_factory` gate. On Windows, a
fully enforced configuration returns a real restricted-process backend; on
unsupported or under-enforced configurations it returns `nullptr`. A dedicated
test runs real Python through this public backend and verifies that public runs
do not carry the internal candidate marker.

`make_execution_backend_registry(...)` now accepts explicit registry options. If
`enable_restricted_process_backend=true` and availability checks pass, the
registry registers a real `restricted_process` factory that can be selected for
enforced filesystem read/write denial and network denial requirements. The
default registry remains descriptor-only for `restricted_process`, so existing
hosts do not accidentally opt into a sandbox policy.

The internal execution plan now also has a fail-closed reparse-point test for
readable roots. If an allowed root contains a symlink-style reparse point that
could redirect traversal outside the granted tree, ACL grant planning fails
with `reparse_point_not_allowed` before any restricted child is launched. A
dedicated junction probe now creates a Windows mount-point reparse point through
`FSCTL_SET_REPARSE_POINT` and verifies the same fail-closed behavior for that
escape shape.

The backend must still remain default-off until host products explicitly choose
the interpreter, workdir, readable roots, writable roots, and UX/policy around
restricted execution.

## Non-Acceptable Shortcuts

- Marking filesystem or network denial as enforced because policy says
  `allow_file_read=false`, `allow_file_write=false`, or `allow_network=false`.
- Returning a `controlled_process_backend` instance from
  `create("restricted_process")`.
- Advertising an AppContainer backend when Python launch works but file or
  network denial has not been tested.
- Relying on lexical path checks alone for code running inside the child
  process.
- Treating an unavailable OS feature as a successful sandbox with degraded
  safety.

## ReArk Impact

Until this backend is implemented and available, ReArk should continue to use
`controlled_process` only for bounded local computation and should not present
it as a secure file or network sandbox.

When `restricted_process` becomes available, ReArk can require:

- `isolation=restricted_process`
- `require_filesystem_read_deny=true`
- `require_filesystem_write_deny=true`
- `require_network_deny=true`

If Wuwe cannot satisfy those requirements, ReArk should deny the risky task or
ask the user to run it in a stronger configured environment instead of falling
back to `controlled_process`.
