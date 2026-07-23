---
id: execution-runtime
title: Controlled execution
description: Run bounded Python subprocesses under explicit policy, approval, and audit controls.
---

# Controlled execution

The execution module exposes Python snippet execution as a policy-bound runtime and optional model tool. It separates authorization from backend enforcement and reports what each backend actually enforces.

## Request and runtime

```cpp
namespace execution = wuwe::agent::execution;

execution::execution_policy policy;
policy.default_workdir = "work";
policy.max_limits.timeout = std::chrono::seconds(3);
policy.max_limits.max_code_bytes = 64 * 1024;
policy.max_limits.max_stdout_bytes = 64 * 1024;
policy.max_limits.max_stderr_bytes = 64 * 1024;

execution::execution_runtime runtime(
  execution::make_controlled_process_backend(),
  policy);

const auto result = runtime.run({
  .language = execution::execution_language::python,
  .code = "print(6 * 7)",
});
```

`execution_request` carries code, stdin, working directory, limits, environment values, shell intent, and metadata. `execution_result` reports exit status, termination reason, timeout or cancellation, truncation, output, diagnostics, elapsed time, and backend metadata.

Only Python is implemented in 0.1.0.

## Policy

`execution_policy` controls:

- allowed languages;
- default working directory;
- readable and writable roots;
- maximum code, input, output, process, memory, CPU, and elapsed limits;
- network, file-read, file-write, and shell permissions;
- approval requirements for network, writes, and shell use;
- allowed environment values.

`execution_runtime` evaluates and normalizes the request, obtains host approval when required, calls the backend, and emits audit events. A request that requires approval is denied when no approval service is configured.

## Model tool

```cpp
execution::execution_tool_provider tools(runtime);
```

The built-in tool is `run_python_snippet`. Its arguments are bounded separately from the execution request, and its timeout must stay within configured limits. Registering the provider with an agent runner does not weaken runtime policy.

## Backends

### `controlled_process`

This is the default and cross-platform backend. It starts a Python subprocess and provides interpreter probing, working-directory selection, environment filtering, timeouts, cancellation, process-tree cleanup where supported, and bounded stdout and stderr.

It is not a strong sandbox. In particular, policy flags such as network or file denial cannot by themselves prevent an ordinary child process from accessing operating-system resources. Use it only for code trusted to the degree appropriate for a controlled subprocess.

### `restricted_process`

The restricted backend is opt-in:

```cpp
execution::execution_backend_registry_options options;
options.enable_restricted_process_backend = true;
auto registry = execution::make_execution_backend_registry(options);
```

On Windows, an explicitly registered restricted backend is available when its configuration passes availability checks. It stages a minimal Python runtime, uses restricted process controls, limits inherited environment, applies filesystem boundaries, and denies network according to its reported enforcement contract.

On non-Windows platforms it is not available in 0.1.0. The default registry publishes the descriptor but does not register the factory, so `controlled_process` remains the default selection.

Container and WebAssembly backends are not implemented.

## Backend selection

`execution_backend_registry` describes backends and selects one only when its `sandbox_enforcement_contract` satisfies the requested requirements. Inspect `sandbox_backend_info` instead of assuming that a backend name implies a particular control.

The package smoke test verifies the default controlled backend, explicit restricted-backend registration behavior, and installed-package execution APIs on Windows and Linux.
