---
id: learning-adaptation
title: Learning and adaptation
description: Build an auditable offline feedback, reward, optimization, evaluation, approval, activation, and rollback loop.
---

# Learning and adaptation

The Learning module is a governed offline adaptation pipeline for Prompt templates, reasoning policies, routing profiles, tool configurations, workflows, model artifacts, and other versioned artifacts. It does not compute model gradients and does not permit hidden self-modification. The independent [SFT and adapter training](training.md) module governs external Full, LoRA, and QLoRA jobs and hands validated model artifacts into this promotion gate.

```text
Experience / Feedback -> Reward Ledger -> Offline Optimizer
        -> Candidate -> Evaluation Gate -> Approval -> Activation / Rollback
```

## Experience and feedback

`experience_record` captures the target being improved, input, observed output, optional expected output, feedback kind, structured feedback, trajectory, source run, and metadata. `experience_store` defines add, get, query, update, and erase operations. `in_memory_experience_store` is a thread-safe reference implementation with target, source, time, metadata, newest-first, and limit filters.

Feedback is evidence, not an automatic instruction to change production behavior. Correction, preference, outcome, positive, and negative records remain distinct through `feedback_kind`.

## Reward ledger

`reward_record` attaches a signed value, non-negative weight, optional named components, objective, source, and provenance to a target and optionally an experience. Negative rewards are valid. Values, weights, and components must be finite.

Wuwe does not infer rewards from arbitrary evidence. Applications own reward semantics, normalization, conflict resolution, and protection against feedback poisoning.

## Artifact registry

`artifact_registry` stages immutable `(target, version)` artifacts, reports the active version, preserves parent-version lineage, and performs explicit activation or rollback. Re-staging identical content is idempotent; reusing the same version for different content is rejected.

`in_memory_artifact_registry` is suitable for tests and embedded prototypes. Production registries should persist activation history and implement deployment-level atomicity appropriate to the artifact type.

## Offline optimizer

Implement `offline_optimizer::optimize()` or use `function_offline_optimizer`. The optimizer receives:

- the Learning request and active Registry baseline;
- target-scoped Experience and Reward records;
- a bounded candidate budget;
- host metadata and cancellation/deadline context.

`make_offline_optimizer_proposer()` adapts that contract to `learning_runner`, bounds the returned candidates, and fills missing target and parent-version lineage. The callback may run local heuristics, prompt search, or a model-based optimizer; Wuwe does not prescribe the optimization algorithm. Do not block this synchronous callback on a long accelerator job. Use `training::training_coordinator` for governed external training and convert the completed job with `learning_candidate_from_training_job()`.

## Promotion gate

`learning_runner` evaluates every bounded candidate against a host evaluator. Promotion policy independently checks absolute score, pass rate, improvement, and regression count. `compare_evaluation_suites()` detects cases that passed in the baseline and failed for the candidate, so an aggregate gain cannot hide a critical regression.

The default `stage_only` mode records an accepted candidate without changing active configuration. `require_approval` consults the host approval service before activation. `trusted_automatic` is an explicit opt-in for a trusted deployment boundary.

When activation is enabled, the runner promotes at most one accepted candidate per target. The default selector prefers candidate score, pass rate, improvement, and fewer regressions. Other accepted candidates are retained as `not_selected`; applications can provide `learning_runner_options::selector` for a domain-specific winner policy.

```cpp
learning::learning_runner runner({
  .proposer = learning::make_offline_optimizer_proposer(
    optimizer, experiences, &rewards, &registry),
  .evaluator = evaluate_candidate,
  .activator = learning::make_registry_only_activator(registry),
  .approvals = &approval_service,
});

const auto result = runner.run({ .target = "support.prompt" }, {
  .policy = {
    .maximum_regressions = 0,
    .activation_mode = learning::learning_activation_mode::require_approval,
  },
});
```

The activator returns the active version, previous version, rollback token, and error information. `make_registry_only_activator()` changes only the registry; a production activator that deploys prompts or policies must make the external change transactional and idempotent.

## Persistence and ownership

The in-memory Experience, Reward, Learning, and Artifact stores are individually thread-safe. They do not provide a cross-store transaction. A production host that requires atomic Experience-plus-Reward import or activation-plus-deployment must supply persistent implementations and a transaction coordinator.

Callbacks receive a stop token and effective deadline. Uncooperative callbacks can be detached and reported, but cannot be forcibly terminated; capture owned values and make optimizer and activator callbacks cancellation-aware.

Observer and common event-sink failures are isolated and counted by default, including after a successful activation. Set `telemetry_failure_mode` to `propagate` only when telemetry failure should fail the caller-visible operation.

See `examples/src/learning_adaptation_example.cpp` for a complete offline candidate promotion loop.
