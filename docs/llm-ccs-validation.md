# CC Switch compatibility review

Reviewed on 2026-09-21 against CC Switch commit
[`8272707d5e2a9be0cd487ff2d3658f0c58121548`](https://github.com/farion1231/cc-switch/tree/8272707d5e2a9be0cd487ff2d3658f0c58121548).
This is a source-contract comparison with local regression tests, not a run of
CC Switch or an authenticated production-service certification.

Sources:

- [OpenClaw presets](https://github.com/farion1231/cc-switch/blob/8272707d5e2a9be0cd487ff2d3658f0c58121548/src/config/openclawProviderPresets.ts)
- [Model discovery service](https://github.com/farion1231/cc-switch/blob/8272707d5e2a9be0cd487ff2d3658f0c58121548/src-tauri/src/services/model_fetch.rs)
- [Model discovery commands](https://github.com/farion1231/cc-switch/blob/8272707d5e2a9be0cd487ff2d3658f0c58121548/src-tauri/src/commands/model_fetch.rs)

## Provider configuration

Compare the effective API root, not whether `/v1` is stored in the base or route.
CCS preset model names, prices and model-level flags are not evidence that every
model supports an adapter capability, or that a discovery endpoint exists.

| Provider | Comparison |
| --- | --- |
| Kimi | Same Moonshot China API root; `.ai` regional override covered |
| MiniMax | Same China API root; `.io` regional override covered |
| SiliconFlow | Same China API root; `.com` regional override covered |
| Doubao | Same Beijing `/api/v3` API root |
| Nvidia | Same `integrate.api.nvidia.com/v1` API root |
| MiMo | Same `api.xiaomimimo.com/v1` API root |
| StepFun | CCS defaults to `/step_plan/v1`; Wuwe defaults to ordinary `/v1`. China/global plan roots work as explicit overrides |
| Zhipu | CCS defaults to `/api/coding/paas/v4`; Wuwe uses ordinary `/api/paas/v4`. Coding root works as an explicit override |
| DeepSeek / OpenRouter | Same effective versioned API roots |

These override checks validate URL construction, not plan entitlement or
whether the upstream exposes a model catalog at every constructed route.
No new providers or default endpoint changes were introduced by this review.

## Discovery behavior

| Area | Finding and disposition |
| --- | --- |
| Authentication | Both support bearer, Anthropic and Gemini key headers. Wuwe also sends the Anthropic version header. CCS requires a key or custom headers; Wuwe permits explicit anonymous configuration and defaults Ollama to no key |
| Standard catalog | Both parse `data[].id`; Wuwe preserves server order and deduplicates, CCS sorts IDs and exposes `owned_by` |
| Zhipu alternate catalog | Gap fixed: accept `models[].slug` in compatible format when `data` is absent. Explicit alternate path required; no Responses generation support implied |
| Malformed catalog | Wuwe rejects invalid IDs/types and missing expected fields. It deliberately does not copy CCS's successful empty result when both catalog fields are absent |
| Alternate routes | CCS tries candidate paths after 404/405 and strips known compatibility suffixes. Wuwe keeps deterministic routing; callers use `models_path` and optionally `format` |
| Full URL / custom headers | CCS exposes full model URL, custom headers and User-Agent. Wuwe discovery options expose a relative path and protocol auth; arbitrary header requirements need a custom transport. No automatic equivalence claimed |
| Pagination and limits | Wuwe follows supported cursors, detects loops, bounds pages/models/body size, and returns all pages or an error. The reviewed CCS service returns its first successful response without pagination |
| Timeout and cancellation | Wuwe has an overall deadline and stop token; reviewed CCS service has a 15-second timeout per candidate request |
| Errors | Both stop on authentication errors; Wuwe returns typed errors without upstream response bodies, while CCS returns redacted/truncated error strings |
| Other model sources | CCS also has OpenCode CLI and OAuth model discovery; these are outside Wuwe's current provider-discovery API |

## Regression evidence

`test_ccs_catalog_compatibility` covers 14 CCS versioned preset/override roots,
standard IDs with vendor metadata, anonymous configuration, the alternate
Zhipu catalog, deduplication, field precedence, malformed slugs and isolation
from native Anthropic parsing.

Existing discovery tests cover all 17 Wuwe registry entries (including the
generic compatible entry and Qwen alias), native formats, pagination, bounds,
cancellation, error classification and local HTTP tests with cpr and httplib.
Provider tests cover generation/streaming behavior and endpoint consistency.

This review validates compatibility contracts. Real credentials, account
permissions, current service availability and model-specific generation behavior
remain outside what source comparison and local fixtures can establish.
