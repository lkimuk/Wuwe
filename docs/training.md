---
id: training
title: SFT and adapter training
description: Build reproducible Agent datasets and govern full, LoRA, or QLoRA jobs through external training providers.
---

# SFT and adapter training

Wuwe provides an independent `wuwe::agent::training` control-plane module for supervised fine-tuning of Agent models. It owns typed Agent examples, dataset lineage, governed job orchestration, model artifact validation, observability, and the explicit handoff into Learning evaluation and promotion. PyTorch, PEFT, distributed execution, CUDA kernels, and accelerator scheduling remain responsibilities of the selected external training system.

```text
Experience / reviewed trajectories
        -> typed Agent SFT dataset + SHA-256
        -> versioned training job state
        -> external provider (TRL, LLaMA-Factory, Axolotl, cloud service)
        -> validated model / adapter artifact
        -> Learning evaluation gate
        -> approval, activation, rollback
```

Training is deliberately separate from `offline_optimizer::optimize()`. Optimizers are synchronous candidate proposers; accelerator training is a long-running external operation that needs stable identity, polling, cancellation, checkpoint recovery, and reconciliation after process failure.

## Training data

`training_example` represents a multi-message Agent trajectory with `system`, `user`, `assistant`, and `tool` roles. Assistant messages explicitly declare whether they contribute to the loss. Tool calls require IDs that are globally unique within the example, object arguments, and immediately following matching Tool results. Invalid or incomplete trajectories fail validation before they reach a provider.

The cross-module `learning::build_sft_dataset()` adapter converts reviewed `experience_record` values into the provider-neutral `wuwe.agent-messages.v1` format. By default it uses `expected_output`, excludes records without a reviewed response, and computes SHA-256 over canonical JSONL. Missing Experience IDs—and mapper-produced examples without IDs—receive deterministic content-derived IDs, so rebuilding identical semantic examples preserves both example identity and dataset digest. Use an `experience_training_mapper` when the source trajectory includes Tool calls or requires domain-specific loss masks. Training itself remains independent of Learning.

```cpp
namespace training = wuwe::agent::training;

auto dataset = learning::build_sft_dataset(experiences.query({
  .target = "support.model",
  .limit = 5'000,
}), {
  .dataset_id = "support-sft",
  .version = "2026-08-13",
  .uri = "s3://datasets/support-sft/2026-08-13.jsonl",
  .system_prompt = "Answer with grounded support procedures.",
  .tokenizer = "Qwen/Qwen3-8B@2f41c0d",
  .chat_template = "qwen3",
});

const auto jsonl = training::export_training_jsonl(dataset);
// The host uploads jsonl to dataset.manifest.uri before submitting training.
```

The dataset URI is a declaration, not an uploader. The host owns storage credentials, retention, consent, redaction, licensing, and the atomic publication of bytes at that URI. A training provider should recompute the declared digest before use.

## Training requests

`training_request` separates the learning objective from the parameter-update strategy. The strategy is a closed `adaptation_config` variant:

- `training_objective::supervised_fine_tuning` describes the objective.
- `full_training_config` carries no adapter-only fields.
- `lora_training_config` requires its LoRA contract.
- `qlora_training_config` requires both LoRA and quantization contracts.

This makes contradictory combinations unrepresentable instead of relying on a discriminator plus optional fields. Requests also pin the base model ID, revision, SHA-256, tokenizer, chat template, dataset version and SHA-256, hyperparameters, resource request, and output URI. Versioned request and artifact codecs require their schema-required fields—including the dataset format, timestamps, hyperparameters, and resources—to be explicit; they reject partial semantic payloads instead of silently restoring current in-memory defaults. A request chooses exactly one training budget: positive `epochs` with zero `maximum_steps`, or zero `epochs` with positive `maximum_steps`. Numeric extension hyperparameters must be finite. Accelerator count, accelerator type, and memory budget are mandatory; maximum runtime may be zero for provider-managed policy but cannot be negative.

The `idempotency_key` is part of the provider protocol. It is scoped locally by `tenant_id` and `workspace_id`: retrying within the same scope with the same key and semantic request returns the same job, while independent scopes may use the same key. Reusing a key within one scope for a changed model, dataset, hyperparameter, resource, or output request must fail.

## Provider contract

Implement `training_provider`, use `function_training_provider` for embedded adapters, or use `http_training_provider` for a remote training service.

```cpp
auto provider = std::make_shared<training::http_training_provider>(
  training::http_training_provider_config {
    .name = "training-service",
    .base_url = "https://training.internal",
    .bearer_token = token,
    .timeout_ms = 30'000,
    .max_response_bytes = 4 * 1024 * 1024,
  });

training::file_training_job_store jobs("state/training-jobs");
training::training_coordinator coordinator(provider, jobs, {
  .approvals = &approval_service,
});
auto submitted = coordinator.submit(request);
if (!submitted) {
  handle_training_error(*submitted.error_if());
}
auto job = *submitted.value_if();
```

The HTTP provider uses protocol version `2026-08-01`. Mutating requests and every response use strict envelopes with `protocolVersion`, `requestId`, operation/status fields, a typed `body`, and mutually exclusive success/error state. A successful response must have a non-null object `body` and no `error`; a failed response must have no business `body` and must provide a typed error with non-empty `code` and `message`, boolean `retryable` when present, and object `details` when present. Errors may also declare `remoteOutcome`. Mutation errors must classify the outcome as `not_applied`, `applied`, or `uncertain`; read-only inspect and lookup errors must use `not_applicable` and cannot claim a remote mutation. HTTP status and `ok` must agree. Snapshot identity, status, progress, and current step are mandatory and type-checked; metric, checkpoint, and artifact timestamps are mandatory whenever those records appear. Valid typed 4xx/5xx envelopes are decoded as their declared training errors; network failures remain `provider_transport`. Unknown versions or error codes, malformed envelopes, inconsistent success flags, operation-inconsistent remote outcomes, and incomplete snapshots are `provider_protocol` failures. Once a complete HTTP response has arrived it is parsed even if cancellation was requested concurrently, because an authoritative remote result must not be discarded.

HTTPS is required by default. Local or otherwise controlled deployments may opt into `http://` only with `allow_insecure_transport = true`. Training HTTP requests never follow redirects: callers must configure the final endpoint so credentials, tenant identity, workspace identity, idempotency keys, and custom headers cannot be forwarded to another origin. Custom headers are allowed only for non-reserved extension fields and cannot override authentication, framing, idempotency, or Wuwe protocol/context headers. Header names must follow the HTTP token grammar, and disallowed HTTP control characters are rejected in bearer tokens, idempotency keys, named context identifiers, and custom header names and values before network access. `max_response_bytes` bounds buffered response bodies and defaults to 4 MiB; exceeding it is a protocol failure and remains remotely uncertain for mutations. Direct HTTP Provider calls validate typed request, checkpoint, and Provider job identities and return `invalid_request` rather than throwing for expected argument failures.

| Operation | Request |
| --- | --- |
| Submit | `POST /v1/training/jobs` with `wuwe.training-request.v1` and `Idempotency-Key` |
| Inspect | `GET /v1/training/jobs/{url-encoded-provider-job-id}` |
| Lookup submission | `GET /v1/training/submissions/{url-encoded-idempotency-key}` |
| Cancel | `POST /v1/training/jobs/{id}/cancel` |
| Resume | `POST /v1/training/jobs/{id}/resume` with a validated checkpoint |

Submit, inspect, cancel, and resume success bodies are Provider snapshots containing `provider_job_id`, `status`, progress, optional steps, metrics, checkpoint, error, metadata, and—only for `succeeded`—a model artifact. Status values are `submitted`, `queued`, `running`, `paused`, `succeeded`, `failed`, or `cancelled`.

Lookup success bodies are `training_submission_record` objects with `idempotency_key`, the original `request_id`, the canonical SHA-256 `request_digest`, and a nested `snapshot`. Reconciliation requires all three identity values to match the caller's request before any local record is created. If an HTTP lookup confirms that a submission exists but its success body cannot be decoded, the Coordinator preserves the applied remote outcome and requires reconciliation instead of treating the response as an ordinary read-only protocol failure. A service must therefore persist the original request identity alongside its idempotency index; returning only a job snapshot is insufficient and intentionally rejected.

The service must authenticate callers, authorize resource use and output locations, enforce quotas, isolate untrusted datasets and training code, protect credentials, and bind idempotency keys to the authenticated tenant. Wuwe does not turn a generic process launcher into a trusted accelerator scheduler.

## Lifecycle and persistence

`training_coordinator` captures a non-empty Provider name at construction and uses that immutable identity for ownership, artifacts, and reconciliation. It validates every provider response and permits only legal state transitions. A provider-reported illegal transition is a `provider_protocol` failure; caller attempts to cancel or resume a terminal job remain `invalid_transition`. Progress and current steps cannot move backwards. Once `total_steps` is declared it cannot change or disappear. A metric step cannot exceed the snapshot's current step. Checkpoints are immutable at an observed step, including identity, URI, digest, timestamp, and metadata; only a checkpoint at a later step may supersede one already persisted. Metric `(step, name)` identities must be unique within each snapshot, and previously observed values, timestamps, and metadata cannot be rewritten. Timestamp identity is compared at the protocol's persisted millisecond precision. Metrics returned incrementally are retained. Successful jobs require a complete artifact whose provider identity, provider job ID, objective, adaptation strategy, base model, tokenizer, chat template, dataset, LoRA configuration, and hyperparameters match the original request.

`file_training_job_store` writes immutable append-only revision files and replays contiguous valid histories on startup. Replay rejects rewritten request or job metadata, changed creation timestamps, backwards update timestamps, timestamps preceding creation, non-contiguous revisions, and invalid lifecycle history. It deliberately does **not** declare durable or atomic mutations: the implementation has process-local locking and rename-based publication, but no directory/file `fsync`, multi-process lock, per-record checksum, compaction, or partial-corruption isolation. Use it for local development and single-process recovery. Production deployments should implement `training_job_store` over a transactional database and declare only capabilities the backend can actually guarantee.

Store reads are typed: `load()` and scoped `find_by_idempotency_key(tenant_id, workspace_id, key)` return `training_result<std::optional<training_job>>`, while `list()` returns `training_result<std::vector<training_job>>`. `create()` returns `training_job_create_result`, whose explicit `inserted` flag distinguishes a newly persisted revision from an idempotent record inserted concurrently by another caller. Tenant and workspace are persisted as immutable job identity. Coordinator load and lifecycle operations require an exactly matching `training_context`; a mismatch is reported as `not_found` to avoid disclosing cross-scope job existence. Empty identifiers form the backward-compatible default scope. Idempotency records are also bound to the frozen Provider name: a Coordinator configured with another Provider cannot silently adopt an existing job. The Coordinator validates all successful custom Store reads and writes at the SPI boundary, including identity, scope, request, provider snapshot, revision, and timestamps; malformed success values become `corrupted_state`, or `reconciliation_required` when a remote mutation has already succeeded. `std::nullopt` means a clean not-found result; `persistence_failure` and `corrupted_state` remain observable instead of being collapsed into not-found. The file store validates and reloads its revision history in the constructor, so construction may throw when the directory cannot be opened or persisted records are malformed; operational reads after successful construction use the typed SPI.

Submission necessarily crosses two systems. `training_error::remote_outcome` explicitly records whether a failure is unrelated to a remote mutation, is known not to have applied it, confirms it was applied, or leaves the result uncertain. Every `training_result::failure()` validates the basic error invariant. Provider and Store boundaries additionally validate the operation effect: read-only operations and Store access require `not_applicable`, while mutation errors must classify a remote outcome. Reconciliation requires `applied` or `uncertain`, while ordinary business errors cannot claim a remote mutation was applied. Invalid custom boundary errors are converted to `provider_protocol` or `corrupted_state` instead of escaping as malformed public results.

If the Provider reports submit success but its snapshot cannot be trusted or the local Store cannot persist it, `submit()` returns `reconciliation_required`, not an ordinary protocol or persistence failure. The same rule applies after successful remote `cancel` or `resume` when the resulting state cannot be validated or committed locally. Transport loss, timeout, cancellation, malformed responses, and Provider exceptions after a mutating call are conservatively treated as uncertain unless the Provider explicitly reports `not_applied`; they also return `reconciliation_required`. Safe details identify the operation, Provider, optional Provider job ID, request and idempotency identity, local revision, remote outcome, and underlying failure. Call `reconcile_submit(request, context)` for an uncertain submit: it performs the Provider's idempotency lookup, verifies the returned idempotency key, original request ID, canonical request digest, snapshot, and lineage, then creates the local job through the same idempotent Store boundary. A found but malformed remote record is still treated as an applied remote outcome and returns `reconciliation_required`; an identity mismatch returns `invalid_request` and is never persisted. Repeating reconciliation returns the existing local job without another Provider lookup. Providers that cannot perform idempotency lookup return `unsupported_operation`. Operators must reconcile before repeating a mutating operation.

The coordinator exposes:

- `submit()` for an idempotent provider submission;
- `reconcile_submit()` for recovering an uncertain submission by Provider idempotency identity;
- `refresh()` for a monotonic provider snapshot; it never resumes or otherwise mutates the remote job, and terminal refresh is an idempotent local read;
- `cancel()` for a provider-owned cancellation; cancelling a terminal job is an invalid transition;
- `resume()` for a paused job with a persisted checkpoint; resuming a terminal job is an invalid transition;
- `load()` for the current persisted local view.

Calls accept the independent `training_context`, including cancellation, deadlines, trace/request/subject identity, tenancy, workspace, and metadata. Named trace, request, subject, tenant, and workspace identifiers propagate into capability requests, approval metadata, audit attributes, and safe HTTP headers/envelope context. Arbitrary context metadata is deliberately not copied into HTTP requests because it may contain sensitive host data. Public coordinator and provider methods return `training_result<T>` with `training_errc`; expected operational failures do not require exception matching. `function_training_provider` converts both standard and unknown exceptions—including exceptions with empty messages—from raw or result-returning callbacks into valid typed failures. HTTP operations use the cancellable transport path; cpp-httplib requests actively interrupt established response-header and response-body waits when the stop token fires, while connection establishment remains bounded by the effective timeout. Cancellation during a mutating transfer is reported with an uncertain remote outcome, while a complete response that has already arrived remains authoritative. An HTTP timeout does not prove the remote operation stopped. Reconcile the same Provider job—or the submit idempotency identity when no job ID was received—before retrying; do not submit a new idempotency key merely because a transport response was lost.

Coordinator observers and common event sinks are best-effort lifecycle telemetry. Their exceptions are isolated after Store commits and can never change an already committed submit, reconciliation, or lifecycle result into a caller-visible failure. Audit evidence remains a separate governance channel.

## Governance

Operations are evaluated before Provider access through the capability names `training.submit`, `training.refresh`, `training.cancel`, `training.resume`, and `training.reconcile`. The default `training_policy` requires approval for submission and checkpoint resume because both can allocate expensive accelerator work. Approval gates fail closed when no service is configured. Controlled environments that already enforce authorization outside the coordinator may opt in explicitly with `trusted_training_policy()`; this is never the default. Audit records distinguish policy evaluation, approval attempts, approvals, and final denied outcomes. Lifecycle telemetry remains separate from audit evidence.

## Evaluation and activation

Training completion does not activate a model. Convert a successful job into a strongly attributed Learning candidate, then evaluate it through the existing regression gate:

```cpp
auto candidate = learning::learning_candidate_from_training_job(job, "adapter-v1");

learning::learning_runner gate({
  .proposer = [candidate](const auto&, const auto&) {
    return std::vector { candidate };
  },
  .evaluator = evaluate_candidate_runtime,
  .activator = deploy_model_artifact,
  .approvals = &approvals,
});
```

The evaluator and activator are deployment-specific because Wuwe cannot infer how a vLLM, TGI, Ollama, Kubernetes, or cloud endpoint loads an adapter. The evaluator should bind requests to the candidate artifact, compare it with the pinned baseline on the same Agent task suite, and tear down the candidate runtime. The activator should make deployment and registry updates idempotent and transactional, then return a rollback token.

## Security and ownership

- Never put bearer tokens, cloud credentials, private dataset contents, or model secrets into job metadata.
- Treat Experience and Tool trajectories as potentially sensitive. Redact and authorize them before dataset publication.
- Verify SHA-256 at each storage boundary; a URI alone is not artifact identity.
- Do not trust provider-supplied lineage. The coordinator validates it against the immutable request.
- Training metrics and checkpoints are evidence, not authority to activate a model.
- Use the Learning approval and regression gate before production activation.
- External training and inference services remain host-managed deployment boundaries.

See `examples/src/training_workflow_example.cpp` for a complete provider-neutral control-plane example.
