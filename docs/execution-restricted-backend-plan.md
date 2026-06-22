# Restricted Execution Backend Plan

Status: design and acceptance record for the future P2 backend. No restricted
backend is available in the current build.

Date: 2026-06-22

## Boundary

`restricted_process` must not be a wrapper around `controlled_process`.

It may become available only when the backend can prove the enforcement it
advertises. Until then, the default registry should expose it as an unavailable
descriptor with planned enforcement fields, and `create("restricted_process")`
must return `nullptr`.

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

The same probe currently records a socket attempt, but it does not prove network
denial on this host. A no-capability AppContainer child reported a pending
nonblocking external connect (`WSAEWOULDBLOCK`) instead of access denied
(`WSAEACCES`). Therefore network denial remains unaccepted, and the public
`restricted_process` backend must stay unavailable.

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
   passes the same checks.

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
and Job Object assignment. It also records that network denial is not yet proven
by the no-capability AppContainer profile on this host, so it is a checkpoint,
not an acceptance pass.

The AppContainer file-boundary probe also proves child-process enforcement for
allowed read, allowed write, denied read, denied write, and `..` traversal
denial within the AppContainer profile storage root. This advances the Windows
candidate materially, but the backend must remain descriptor-only until network
denial and the final runtime launch contract are proven in the same style.

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
