---
id: security-governance
title: Security and governance
description: Apply explicit capability decisions, host approvals, audit records, and backend enforcement contracts.
---

# Security and governance

Wuwe separates four concerns that are often conflated: declaring a sensitive capability, deciding whether policy permits it, obtaining host approval, and reporting what an execution backend actually enforces.

## Capability policy

`capability_request` describes the requested action, risk, affected resources, tool, trace, subject, and host metadata. Built-in capability names cover Python, argv-based process and shell execution, filesystem reads and writes, outbound networking, environment access, secret access, learned-artifact activation, and effectful exploration execution.

Policy evaluation returns a `capability_policy_result` with one of three decisions:

- `allow` permits the request;
- `deny` rejects it with a reason;
- `require_approval` tells the caller to consult the configured approval boundary.

The result is an explicit decision contract. It is not a global identity system or RBAC engine; the host remains responsible for mapping users, sessions, workspaces, and product policy into those decisions.

## Approvals

Applications integrate human or service authorization by implementing `approval_service`:

```cpp
class product_approval_service final
    : public wuwe::agent::approval::approval_service {
public:
  wuwe::agent::approval::approval_decision decide(
      const wuwe::agent::approval::approval_request& request) override {
    // Present the request through the product's own UI or policy service.
    return {
      .kind = wuwe::agent::approval::approval_decision_kind::denied,
      .scope = wuwe::agent::approval::approval_scope::once,
      .reason = "no product approval was recorded",
    };
  }
};
```

Decisions can be approved, denied, or sent for manual review, with once, session, or workspace scope. Scope is descriptive data for the host to enforce; Wuwe does not persist product authorization automatically.

`deny_all_approval_service` is a safe default. `allow_all_approval_service` is intended for tests or an explicitly trusted environment, not as a production default. Controlled execution rejects requests that require approval when no approval service is configured. Learning & Adaptation leaves an accepted artifact in `approval_required`, and Exploration & Discovery leaves the experiment unexecuted, when their policies require approval but no service is available. Imported exploration evidence does not create rewards unless the host provides an explicit mapper.

## Audit

`audit_event` records the module, event name, identifiers, outcome, timestamp, elapsed time, and structured attributes. The common `audit_sink` interface currently includes an in-memory implementation suitable for tests and host adaptation.

```cpp
namespace approval = wuwe::agent::approval;
namespace audit = wuwe::agent::audit;
namespace execution = wuwe::agent::execution;

audit::in_memory_audit_sink audit_log;
approval::deny_all_approval_service approvals;

execution::execution_runtime runtime(
  execution::make_controlled_process_backend(),
  policy,
  &audit_log,
  &approvals);
```

The execution runtime records policy, approval, start, completion, failure, cancellation, and timeout outcomes where applicable. Memory and MCP also expose module-specific audit callbacks; they are not automatically merged into the common execution audit sink.

The host owns durable storage, redaction, access control, retention, and export of audit data.

## Content trust and retrieval isolation

`content_provenance` labels content by trust level and source. Memory and Knowledge
set conservative provenance when a host has not supplied one. Their default request
augmentation keeps retrieved material out of system messages, preserves leading
system instructions, and escapes boundary delimiters before rendering the material
inside `<wuwe-context>`.

The keys `trust`, `source`, `source_id`, and `source_uri` are reserved inside
`content_provenance::metadata`; extension metadata cannot overwrite their
authoritative `wuwe.content.*` values. Reapplying provenance also removes stale
optional source identifiers when the new provenance does not provide them.

`content_trust_guardrail` can enforce this contract at input, retrieval, tool-output,
and memory-write stages. It fails closed on missing or unknown trust labels and
denies promotion of non-system/non-application content to the system role. This is
a defense-in-depth boundary, not a replacement for capability authorization and
tenant-aware retrieval filtering.

Knowledge high-level APIs use strict ACL defaults. Unlabeled and empty-labeled
records are denied; explicitly public or matching tenant/user/role labels are
required. Identity comes from the host execution context by default, not from model
tool arguments.

Skills use the same provenance boundary. A local Skill package is untrusted by
default even when all SHA-256 checks pass: integrity does not establish publisher
authenticity. Only a host source policy may promote trust. Untrusted Skill
instructions are rendered as escaped data rather than system instructions, and
Skill capability declarations never create authorization. Script resources are
loaded as inert verified bytes and are never executed by the Skills module. See
[Skills](skills.md) for loader enforcement and the local-directory TOCTOU limit.

## Sandbox enforcement contracts

`sandbox_backend_info` reports backend availability, isolation level, features, and a field-by-field `sandbox_enforcement_contract`. Enforcement levels distinguish controls that are enforced, partial, not enforced, not applicable, or planned.

Portable sandbox intent now lives in `sandbox_policy`. A backend must compile it into a
`sandbox_plan`; invalid policies, unavailable platforms, isolation mismatches, and missing
field-level enforcement return typed failures and structured blockers before launch. Filtered
network access is reported independently from complete network denial, so a deny-only backend
cannot overstate its capability.

Treat this contract as capability reporting, not as a guarantee inferred from a backend name. The default `controlled_process` backend bounds subprocess operation but is not a strong sandbox. The Windows and macOS restricted backends provide stronger controls when explicitly enabled and available. Linux reports that backend as unavailable instead of silently weakening the requested policy.

The Windows restricted backend now executes only a private, versioned native plan produced by its
compiler. Its filesystem implementation rejects reparse-point traversal and files with external hard
links, locks every ancestor handle during security-sensitive path operations, applies precedence with
protected DACLs, overrides broad application-package grants on explicit protected/denied paths,
withholds ACL-owner and parent-delete authority from writable roots, and retries restoration without
discarding pending recovery state. Native plans explicitly disclose intrinsic Windows AppContainer
filesystem defaults and reject policies that forbid them. Policy resource limits are
enforced as upper bounds rather than reported as unsupported metadata. Unsupported network modes
are rejected before launch instead of being approximated.

The macOS restricted backend accepts only private native plans compiled for its
Seatbelt launcher. It binds the selected Python interpreter and framework runtime,
uses a deny-default filesystem and network profile, closes unrelated descriptors,
and applies process-group lifecycle and per-process CPU safeguards. Policies that
require filtered networking, local binding, a complete virtual-memory ceiling, or
a per-tree process-count limit are rejected rather than overstated.

Container and WebAssembly appear as isolation categories in the public contract but are not implemented backends in version 1.0.0.

See [Sandbox architecture](sandbox-architecture.md) for the portable policy and plan contract, and
[Controlled execution](execution-runtime.md) for execution authorization, backend selection, and
the currently verified platform boundary.
