# ReArk Integration Notes

This note summarizes the Wuwe packaging and document-parser runtime changes
that ReArk should account for when updating its Wuwe integration.

## What Changed

Wuwe now ships as one complete package named `wuwe`.

The older split-package model (`wuwe-core` and `wuwe-full`) is intentionally no
longer the public release path. The package includes the C++ SDK, examples,
documentation, and bundled runtime sidecars needed by the default RAG document
loader.

The document parser is now an implementation detail. ReArk should not ask users
to configure Apache Tika, Java, a parser URL, or a runtime directory.

## Recent Wuwe Changes ReArk Should Notice

### Agent Runtime

Wuwe now has embeddable agent runtime APIs for host applications. ReArk should
prefer the runtime runner APIs when embedding agent behavior instead of wiring
LLM calls, tools, streaming, cancellation, and final-result callbacks by hand.

Relevant capabilities:

- synchronous and async agent execution,
- cooperative cancellation with `std::stop_token`,
- tool lifecycle callbacks,
- final-result callbacks,
- stateful tool providers whose private state stays outside the model-visible
  schema.

### OpenAI-Compatible Streaming

The LLM layer now supports true OpenAI-compatible streaming. ReArk can render
partial model output as it arrives instead of waiting for the final response.

Integration expectation:

- use streaming callbacks/event handling for interactive chat or long-running
  agent tasks,
- preserve cancellation behavior during streaming,
- avoid building a separate ad hoc streaming parser in ReArk when Wuwe already
  exposes the runtime surface.

### LLM Provider Registry And Factory

Wuwe now exposes a narrow provider registry and factory surface for host
applications. ReArk should use these headers for model-provider settings and
provider construction:

```cpp
#include <wuwe/agent/llm/llm_provider_registry.h>
#include <wuwe/agent/llm/llm_provider_factory.h>
```

Use `list_llm_providers()`, `find_llm_provider()`,
`make_default_llm_config()`, and `normalize_llm_client_config()` to populate UI
state and apply Wuwe-owned defaults. Use `make_llm_client()` or
`llm_client_factory` to construct the selected provider.

ReArk should avoid including `<wuwe/wuwe.h>` in provider-selection translation
units. The aggregation header remains available for convenience, but Wuwe no
longer performs LLM factory registration from that public header. This keeps
host code from pulling registration side effects into arbitrary translation
units.

### Reasoning Facade

Wuwe now includes a policy-driven Reasoning facade for selecting standard
agentic reasoning patterns over the lower-level runtime modules.

Available strategy families include:

- simple single-pass execution,
- ReAct-style tool use,
- reflect-and-retry self-correction,
- plan-and-execute workflows.

ReArk should use the Reasoning facade when it wants a higher-level agentic
workflow rather than manually coordinating prompts, tools, reflection, and plan
steps.

The facade emits one unified event stream and returns structured traces with
budget usage counters, which are useful for ReArk UI timelines, logs, debugging,
and audit views.

Wuwe Reasoning now exposes a native async runner API. ReArk should prefer
`reasoning_runner::run_async(...)` for interactive UI flows instead of wrapping
`reasoning_runner::run(...)` in its own compatibility bridge.

Relevant async capabilities:

- `reasoning_run::request_stop()`,
- `reasoning_run::wait()`,
- `reasoning_run::get()`,
- callback hooks for `on_event`, `on_delta`, `on_done`, `on_error`, and
  `on_cancelled`,
- cancellation through either a caller-provided `std::stop_token` or the run
  handle,
- stable `reasoning_error_code` values with underlying LLM/provider errors
  preserved on the result.

Reasoning events, streaming deltas, trace updates, and final results are still
emitted from the runner's execution thread. ReArk should marshal those
callbacks back to its UI thread before updating UI state.

ReArk should also prefer Wuwe's default builders for common workflows:

```cpp
auto runner = wuwe::agent::reasoning::make_default_agentic_runner(
  client,
  provider,
  {
    .model = model,
    .memory = memory,
  });
```

For RAG-backed reasoning, prefer:

```cpp
auto runner = wuwe::agent::reasoning::make_knowledge_aware_runner(
  client,
  retriever,
  {
    .model = model,
    .memory = memory,
  });
```

These helpers reduce ReArk-side glue for ReAct, Reflection, Planning, Memory,
Knowledge Retrieval, streaming, cancellation, trace, and budget reporting.

Reasoning traces and usage counters can now be exported with:

```cpp
auto trace_json = wuwe::agent::reasoning::reasoning_trace_to_json(result.trace);
auto usage_json = wuwe::agent::reasoning::reasoning_usage_to_json(result.usage);
auto result_json = wuwe::agent::reasoning::reasoning_result_to_json(result);
```

ReArk can now compose its own Hyle/source/decompile/signature provider with the
standard Wuwe knowledge provider instead of writing a hand-rolled forwarding
bridge:

```cpp
auto tools = wuwe::compose_tool_providers(reark_tools, knowledge_tools);
auto runner = wuwe::agent::reasoning::make_default_agentic_runner(
  client,
  tools,
  options);
```

Provider order is the override policy: when two providers expose the same tool
name, the earlier provider wins. This lets ReArk keep product-specific tools in
front while still using Wuwe's standard `search_knowledge` provider.

The composed provider remains cancellation-aware. In both ReAct-style runs and
Plan/Execute tool steps, Wuwe forwards the active `std::stop_token` to providers
that implement `invoke(name, arguments_json, stop_token)`.

Public Wuwe headers no longer expose `emit(...)` as a method name in the
Reasoning/Planning/Reflection event path, reducing Qt macro friction for ReArk.

### Knowledge And URL Loading

The Knowledge Retrieval module now supports local files, directories, URL/HTML
loading, richer document parsing through the bundled runtime, cited chunking,
retrieval, grounding checks, and `search_knowledge` tool exposure.

ReArk should treat `knowledge_document_loader::make_default()` as the standard
ingestion entry point for mixed local document and URL workflows.

## Default Document Loading

Use the default loader with no parser arguments:

```cpp
auto loader = wuwe::agent::knowledge::knowledge_document_loader::make_default();
```

Do not use the old pattern:

```cpp
// Old integration style. Do not use.
auto loader =
  wuwe::agent::knowledge::knowledge_document_loader::make_default(tika_url);
```

The default loader automatically discovers the package-layout runtime directory
and starts the bundled parser when needed.

## Runtime Layout ReArk Should Ship

ReArk previously consumed Wuwe directly from the CMake install prefix. That
remains supported, and the install prefix now carries the bundled runtime too.

When ReArk packages or embeds Wuwe from an install directory, keep this layout
available next to the ReArk executable or its working directory:

```text
runtime/
  tika/
    tika-server-standard.jar
  jre/
    bin/
      java.exe
```

The runtime discovery path checks package-style `runtime/tika` locations next to
the current working directory and executable. If ReArk repackages files into its
own installer, it should preserve that `runtime` directory as a unit.

If ReArk points directly at a Wuwe install prefix during development, verify
that the prefix contains the same `runtime` directory:

```powershell
cmake --install build --config Release --prefix install
Test-Path install\runtime\tika\tika-server-standard.jar
Test-Path install\runtime\jre\bin\java.exe
```

## Removed User-Facing Configuration

ReArk should remove UI, config, docs, or launch scripts that mention:

- `WUWE_TIKA_URL`
- `WUWE_TIKA_RUNTIME_DIR`
- `WUWE_TIKA_AUTO_START`
- `--tika-url`
- manual Tika startup instructions
- choosing between `wuwe-core` and `wuwe-full`

These are no longer part of the simple user path.

## Packaging Command

Generate the Wuwe release package with:

```powershell
.\tools\package-wuwe.ps1 -Configuration Release
```

The expected output is:

```text
dist/wuwe-<version>-windows-x64.zip
```

The generated package manifest records the bundled parser and JRE checksums.

For direct install-prefix consumption, use:

```powershell
cmake --install build --config Release --prefix install
```

The install prefix is expected to be self-contained for ReArk development and
installer staging.

## Verification Signals

Before updating ReArk to a new Wuwe package, verify:

- `manifest.json` has `"name": "wuwe"`.
- `runtime/tika/tika-server-standard.jar` exists.
- `runtime/jre/bin/java.exe` exists on Windows x64 packages.
- The same runtime files exist when ReArk consumes a raw CMake install prefix.
- ReArk can create `knowledge_document_loader::make_default()` without any
  parser URL configuration.
- PDF or Office document ingestion works without asking the user to start or
  configure Tika.

## Intent

The integration goal is a zero-configuration user experience: ReArk users should
interact with document ingestion and RAG features, not with the parser runtime
behind them.
