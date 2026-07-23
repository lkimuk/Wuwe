---
id: http-backends
title: HTTP backends
description: Configure Wuwe's common HTTP transport and TLS behavior.
---

# HTTP backends

Wuwe exposes one `http_client` interface for provider clients, embeddings, remote indexes, and MCP HTTP integration.

## Implementations

| Backend | Role | HTTPS |
| --- | --- | --- |
| `cpr_http_client` | Default backend, built on cpr/libcurl | Schannel on the default Windows profile; OpenSSL on Linux and OpenSSL builds |
| `httplib_http_client` | Alternate cpp-httplib implementation | Requires an OpenSSL-enabled build and `WUWE_ENABLE_HTTPLIB_SSL=ON` |

Select the default implementation at configure time:

```powershell
cmake --preset windows-vcpkg -DWUWE_HTTP_BACKEND=cpr
cmake --preset windows-vcpkg -DWUWE_HTTP_BACKEND=httplib
```

Both implementations are public classes; `default_http_client` uses the backend selected for the build.

## Common request model

```cpp
wuwe::http_request request {
  .method = "GET",
  .url = "https://example.com/data",
  .headers = { { "Accept", "application/json" } },
  .timeouts = {
    .total_ms = 30000,
    .connect_ms = 5000,
  },
  .follow_redirects = true,
  .trace_id = "request-42",
};

wuwe::default_http_client client;
const auto response = client.send(request);
```

`http_request` supports headers, body, redirects, proxy credentials, TLS verification and custom CA paths, total/connect/read/write timeouts, and a trace ID. `http_response` returns normalized errors, transport errors, status, headers, and body.

`send_stream()` delivers response chunks through a callback and accepts a `std::stop_token`. Returning `false` from the callback cancels the transfer.

## TLS rules

- The standard Windows package uses Schannel and the Windows certificate store.
- The Linux release package links OpenSSL.
- cpp-httplib HTTPS is available only when OpenSSL was linked.
- Disabling peer or host verification is an explicit per-request choice and should be limited to controlled development environments.

See [Dependencies](dependencies.md) for build options and installed-package requirements.

## Verification

CI builds and tests both HTTP backend selections on Windows. The Linux release job verifies the OpenSSL profile. Network-independent tests cover requests, redirects, proxying, streaming, cancellation, error classification, headers, and TLS behavior where available.
