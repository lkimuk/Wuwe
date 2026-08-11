---
id: migration-1.0
title: Migrating to 1.0
sidebar_position: 4
description: Upgrade a Wuwe v0.1.0 consumer to the stable 1.0.0 SDK.
---

# Migrating to Wuwe 1.0

Wuwe 1.0.0 is a major framework expansion over the tagged v0.1.0 release. The
intermediate 0.2.0 identity was used during development and was not published as
a repository release tag.

## Required upgrade steps

1. Remove the old build directory and any cached Wuwe package path.
2. Install the complete 1.0.0 SDK so its headers, library, generated version
   header, CMake package, and runtime sidecars come from the same archive.
3. Change CMake consumers to `find_package(wuwe 1 CONFIG REQUIRED)`.
4. Reconfigure and rebuild every binary that links Wuwe.
5. Re-run application integration, security, persistence, cancellation, and
   trajectory regression tests.

Do not mix v0.1.0 and 1.0.0 headers or libraries. Wuwe promises 1.x source
compatibility after recompilation, not cross-release C++ ABI compatibility.

## Integration changes to review

- Prefer the generated `<wuwe/version.hpp>` header for runtime and compile-time
  version checks.
- Use the exported `wuwe::wuwe` CMake target; it publishes the C++20 requirement
  and enabled public dependencies.
- Review provider capability metadata before relying on streaming, structured
  output, reasoning controls, or detailed usage accounting.
- Treat filesystem roots, process allowlists, approvals, and capability policy
  as required host configuration rather than sample defaults.
- Use durable Run, Store, Host, audit, and observability contracts for long-lived
  or resumable work instead of retaining process-local callbacks as state.
- Inspect `storage_capabilities()` before assuming transactions, migrations,
  multi-process coordination, or distributed consistency.
- If adopting Skills, use the authoritative `skill.json` format and register only
  immutable loaded packages. Do not treat legacy Multi-Agent or A2A skill
  descriptors as executable packages, and do not promote local package trust from
  a matching hash alone.

## Execution boundary changes

`controlled_process` is the portable default backend. It applies policy,
timeouts, cancellation, output bounds, and process cleanup, but it is not a
strong security sandbox. The opt-in `restricted_process` backend is certified
on Windows and macOS, using AppContainer and Seatbelt respectively. Linux
reports the stronger backend as unavailable instead of weakening the requested
policy. Container and WebAssembly backends are not implemented in 1.0.0.

## Packaging changes

Official Windows, Linux, and macOS packages contain a manifest, checksums, public
headers, the static SDK, CMake metadata, documentation, examples, and optional
Tika/Temurin sidecars. SQLite and OpenSSL remain public link dependencies when
enabled. Use the package manifest as the source of truth for a particular
archive.

## Persistence rollout

Back up production data before the first 1.0.0 deployment. Exercise SQLite
migration and rollback with representative data. File-based stores do not claim
transactions, multi-process coordination, or automatic schema migration.
