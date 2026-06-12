# HTTP Backends

Wuwe keeps outbound HTTP behind the `wuwe::http_client` interface. The framework
now provides two concrete implementations:

- `wuwe::cpr_http_client`: cpr/libcurl backend. This remains the default.
- `wuwe::httplib_http_client`: `cpp-httplib` backend for local HTTP, comparison
  testing, and simple dependency experiments.

`wuwe::default_http_client` is a small compile-time selected facade over one of
those implementations.

## Design Principles

- Keep `http_client` as the only contract consumed by LLM, RAG, memory, Tika,
  Qdrant, and host applications.
- Preserve source compatibility for existing callers that only check
  `http_response::error_code`.
- Put portable behavior in the shared request/response structs instead of
  leaking cpr or cpp-httplib types into public agent APIs.
- Allow explicit backend construction for A/B debugging and production
  workarounds.
- Keep live network smoke tests opt-in; deterministic local tests must cover the
  default contract.
- Prefer cpr/libcurl as the default external-provider transport until another
  backend proves equivalent in real proxy, TLS, and platform certificate
  environments.

## Selecting The Default Backend

Configure with:

```powershell
cmake -S . -B build
```

The default is:

```text
WUWE_HTTP_BACKEND=cpr
```

Switch the default implementation with:

```powershell
cmake -S . -B build-http-httplib -DWUWE_HTTP_BACKEND=httplib
```

Existing code that constructs `default_http_client` does not need to change.

## Explicit A/B Testing

Callers can bypass the default facade and construct either backend directly:

```cpp
std::shared_ptr<wuwe::http_client> cpr =
  std::make_shared<wuwe::cpr_http_client>();

std::shared_ptr<wuwe::http_client> httplib =
  std::make_shared<wuwe::httplib_http_client>();
```

This is useful when comparing provider behavior, isolating network bugs, or
checking whether a failure is backend-specific.

## Capability Notes

Both backends support the shared Wuwe `http_client` contract:

- request method, URL, headers, body, and legacy total timeout,
- split timeout options for total/connect/read/write where the backend supports
  them,
- redirect control,
- proxy configuration,
- TLS peer/host verification control and custom CA path configuration,
- trace id propagation through `X-Request-Id` when the caller did not set that
  header explicitly,
- response body, status code, response headers, and transport error reporting,
- `send_stream(...)` chunk callback,
- cooperative cancellation through `std::stop_token`,
- callback-driven stream abort.

`http_response::error_code` is kept as the compatibility field used by existing
callers. It contains a transport error when the request could not complete, or
an HTTP status error for non-2xx responses. New code should prefer the more
explicit fields:

```cpp
if (response.transport_error) {
  // DNS, connect, TLS, timeout, callback abort, etc.
}
else if (response.status_code >= 400) {
  // Server returned an HTTP error response.
}
```

Response headers are available through `response.headers`; this is useful for
provider request ids, rate-limit headers, `Retry-After`, and diagnostics.
Use `find_http_header(response.headers, "retry-after")` or
`has_http_header(...)` for case-insensitive lookup.

The cpr/libcurl backend is still the recommended default for external HTTPS
providers because it has mature TLS, proxy, redirect, and platform certificate
behavior.

The `cpp-httplib` backend is excellent for local HTTP services such as Tika,
Qdrant, MCP test servers, and backend comparison. HTTPS support is enabled when
`WUWE_ENABLE_HTTPLIB_SSL=ON` and CMake can find OpenSSL. If OpenSSL is not
available, the httplib backend remains HTTP-capable but cannot be used for HTTPS
LLM providers.

### Backend Mapping

| Capability | cpr/libcurl | cpp-httplib |
| --- | --- | --- |
| Plain HTTP | Yes | Yes |
| HTTPS | Yes | Yes when OpenSSL is found |
| Total timeout | Yes | Yes |
| Connect timeout | Yes | Yes |
| Read/write timeout | Uses libcurl total/connect behavior | Yes |
| Follow redirects | Yes | Yes |
| Max redirects | Yes | Yes |
| Proxy URL | Yes | Yes for host/port proxies |
| Proxy basic auth | Yes | Yes |
| Proxy bearer token | Not exposed by current cpr mapping | Yes |
| TLS verify peer/host | Yes, separately | Mapped to one server-certificate verification switch |
| Custom CA file/dir | Yes | Yes when OpenSSL is enabled |
| Response headers | Yes | Yes |
| Streaming callback abort | Yes | Yes |

When a backend cannot express a lower-level option exactly, Wuwe keeps the
shared contract stable and maps to the closest supported behavior. For example,
cpr/libcurl exposes total and connect timeout controls through this layer, while
cpp-httplib exposes connect/read/write controls directly. Likewise,
cpp-httplib maps `verify_peer=false` or `verify_host=false` to disabling server
certificate verification for that request.

When `openssl` is available on `PATH`, Wuwe's CMake configuration attempts to
infer `OPENSSL_ROOT_DIR` from that executable before calling
`find_package(OpenSSL)`. If OpenSSL is installed somewhere unusual, callers can
still pass it explicitly:

```powershell
cmake -S . -B build-http-httplib `
  -DWUWE_HTTP_BACKEND=httplib `
  -DOPENSSL_ROOT_DIR="D:\softwares\OpenSSL 3.5 LTS"
```

## Verification

Run the normal default-backend tests:

```powershell
cmake --build build --config Debug --target net_tests
ctest --test-dir build -C Debug -R net_tests --output-on-failure
```

Verify the alternate default backend:

```powershell
cmake -S . -B build-http-httplib -DWUWE_HTTP_BACKEND=httplib
cmake --build build-http-httplib --config Debug --target net_tests
ctest --test-dir build-http-httplib -C Debug -R net_tests --output-on-failure
```

`net_tests` exercises `cpr_http_client`, `httplib_http_client`, and
`default_http_client` against the same local HTTP server.

The test covers:

- GET and POST requests,
- response status and response headers,
- case-insensitive response-header lookup helpers,
- trace id propagation,
- redirect following and disabled redirects,
- HTTP proxy routing through a local proxy server,
- stream callback delivery and callback abort,
- cooperative stop-token cancellation,
- malformed URL and unsupported method handling,
- closed-port transport failure classification,
- TLS verification failure classification when OpenSSL is available,
- self-signed HTTPS success when verification is explicitly disabled.

For release confidence, run both backend configurations:

```powershell
ctest --test-dir build -C Debug --output-on-failure
ctest --test-dir build-http-httplib -C Debug -R net_tests --output-on-failure
```

Optional live HTTPS smoke testing is available by setting:

```powershell
$env:WUWE_TEST_HTTPS_URL = "https://example.com/"
ctest --test-dir build -C Debug -R net_tests --output-on-failure
ctest --test-dir build-http-httplib -C Debug -R net_tests --output-on-failure
```

Live tests are intentionally opt-in so normal CI remains deterministic and does
not depend on public network access.
