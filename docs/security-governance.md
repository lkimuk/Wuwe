---
id: security-governance
title: Security and governance
description: Apply explicit capability decisions, host approvals, audit records, and backend enforcement contracts.
---

# Security and governance

Wuwe separates four concerns that are often conflated: declaring a sensitive capability, deciding whether policy permits it, obtaining host approval, and reporting what an execution backend actually enforces.

## Capability policy

`capability_request` describes the requested action, risk, affected resources, tool, trace, subject, and host metadata. Built-in capability names cover Python and shell processes, filesystem reads and writes, outbound networking, environment access, and secret access.

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

`deny_all_approval_service` is a safe default. `allow_all_approval_service` is intended for tests or an explicitly trusted environment, not as a production default. Controlled execution rejects requests that require approval when no approval service is configured.

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

## Sandbox enforcement contracts

`sandbox_backend_info` reports backend availability, isolation level, features, and a field-by-field `sandbox_enforcement_contract`. Enforcement levels distinguish controls that are enforced, partial, not enforced, not applicable, or planned.

Treat this contract as capability reporting, not as a guarantee inferred from a backend name. The default `controlled_process` backend bounds subprocess operation but is not a strong sandbox. The Windows-only restricted backend provides stronger controls when explicitly enabled and available.

Container and WebAssembly appear as isolation categories in the public contract but are not implemented backends in version 0.1.0.

See [Controlled execution](execution-runtime.md) for policy fields, backend selection, and the verified platform boundary.
