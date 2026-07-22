# Dependencies

Wuwe separates build/link dependencies from optional runtime services and
bundled runtime sidecars. This distinction matters because each release archive
contains a static C++ SDK: an application linking that SDK must still
make enabled public link dependencies available to CMake.

## Release profiles

The official Windows and Linux x64 presets for 0.1.0 use reproducible,
platform-specific profiles:

| Capability | Release selection | Notes |
| --- | --- | --- |
| Default HTTP client | cpr/libcurl | Used for external LLM and embedding providers. |
| TLS | Schannel on Windows; OpenSSL on Linux | Linux restores pinned OpenSSL through vcpkg. Windows uses the native certificate store and does not link OpenSSL. |
| cpp-httplib HTTPS | Disabled on Windows; enabled on Linux | HTTPS support follows whether the release profile links OpenSSL. |
| SQLite | Required | Enables local memory persistence and the SQLite-backed knowledge index. |
| Tika and Java | Bundled sidecars | Used for PDF and Office parsing. |
| Qdrant | Optional runtime service | Accessed over HTTP; no Qdrant client library is linked. |

The generated `manifest.json` records the resolved HTTP, TLS, OpenSSL, and
SQLite capabilities of each package. Do not infer package capabilities from the
machine on which an application is running.

The presets expect `VCPKG_ROOT` to point to a vcpkg installation:

```powershell
$env:VCPKG_ROOT = "D:\tools\vcpkg"
cmake --preset windows-vcpkg
```

```bash
export VCPKG_ROOT="$HOME/vcpkg"
cmake --preset linux-vcpkg
```

The configure step reads the repository's `vcpkg.json`, resolves dependency
versions from the pinned `builtin-baseline`, and restores missing packages into
the selected build directory. It does not install libraries globally. A clean
machine and an established developer machine therefore resolve the same SQLite
and OpenSSL port versions for a given triplet.

## Configure options

### TLS

Use `WUWE_TLS_BACKEND` to select TLS behavior:

| Value | Behavior |
| --- | --- |
| `native` | Use the platform/libcurl-native TLS backend. On Windows this explicitly selects Schannel. |
| `openssl` | Require OpenSSL at configure time and use it for cpr/libcurl and cpp-httplib HTTPS. |
| `auto` | Use OpenSSL when found; otherwise use the native backend. Intended for local development, not official release builds. |

Example OpenSSL build:

```powershell
cmake --preset windows-vcpkg-openssl
cmake --build --preset windows-vcpkg-openssl-release
```

The `linux-vcpkg` preset always selects OpenSSL because it is the certified
Linux TLS profile.

The OpenSSL preset enables the manifest's `openssl` feature. vcpkg restores the
pinned development package before CMake configures Wuwe. When `openssl` is
selected outside that preset, configuration still fails immediately if the
OpenSSL development package is unavailable. An installed OpenSSL-enabled Wuwe
package calls `find_dependency(OpenSSL REQUIRED)` for consuming projects.

`WUWE_ENABLE_OPENSSL` is retained only as a compatibility input for old build
directories. New builds should use `WUWE_TLS_BACKEND`.

### SQLite

Use `WUWE_SQLITE_MODE` to control SQLite:

| Value | Behavior |
| --- | --- |
| `on` | Require SQLite3; configuration fails when it cannot be found. Used by the official release preset. |
| `auto` | Enable SQLite when found and otherwise build without it. Convenient for local development. |
| `off` | Build without SQLite capabilities. |

The repository manifest declares SQLite as a default dependency. The official
preset restores it automatically and uses the CMake `SQLite::SQLite3` target.
Non-manifest builds can also provide either `SQLite::SQLite3` or vcpkg's
`unofficial::sqlite3::sqlite3` target. The selected dependency is recorded in
the installed package configuration and release manifest.

`WUWE_ENABLE_SQLITE` is retained only as a compatibility input for old build
directories. New builds should use `WUWE_SQLITE_MODE`.

## Dependency inventory

| Dependency | Category | Required when | Delivery |
| --- | --- | --- | --- |
| C++20 compiler and CMake | Build | Always | Provided by the developer/build environment. |
| cpr/libcurl | Build/link | Always in 0.1.0 | Fetched at the pinned cpr revision and installed with Wuwe's CMake package. |
| cpp-httplib | Source/build | Always | Checked-in header used by the alternate HTTP client and MCP listener. |
| OpenSSL | Build/link/runtime | `WUWE_TLS_BACKEND=openssl` | Supplied by the build and consuming application environment; not bundled by Wuwe. |
| SQLite3 | Build/link/runtime | `WUWE_SQLITE_MODE=on`, or detected in `auto` | Automatically restored by `vcpkg.json` for official builds; SDK consumers must provide a compatible package. |
| nlohmann/json | Source/build | Always | Checked-in third-party headers. |
| Apache Tika Server | Runtime sidecar | Default document parsing | Pinned jar bundled in the release package. |
| Java runtime | Runtime sidecar | Bundled Tika on Windows/Linux x64 | A pinned, platform-specific JRE is selected for each package. |
| Qdrant | Optional service | Remote vector indexing is configured | Deployed separately and accessed through REST. |

The bundled curl build disables opportunistic Brotli and zstd discovery. This
prevents unrelated packages installed on a build machine from silently changing
the static SDK's link interface. Standard gzip/deflate HTTP decoding remains
available through curl's pinned zlib dependency.

## SQLite capability boundary

SQLite support in 0.1.0 provides:

- `sqlite_memory_store` for durable memory CRUD and scoped local queries;
- `sqlite_knowledge_index` for durable chunk and embedding storage;
- transactional knowledge batch upsert and deterministic persistence tests.

It is intentionally a local, single-process-oriented persistence option. The
current memory search applies scope in SQL and performs some filtering and
lexical ranking in C++. The knowledge index stores embeddings as JSON and
performs cosine similarity with a linear scan in C++; it is not a SQLite vector
extension or an approximate-nearest-neighbor index.

For large semantic indexes, multi-process write workloads, or server
deployment, use an application database adapter and/or Qdrant. FTS5, native
SQLite vector extensions, WAL tuning, schema migrations, and stronger
multi-process coordination are future capabilities rather than 0.1.0 claims.

## Consuming an installed package

Consumers use the normal CMake package:

```cmake
find_package(wuwe CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE wuwe::wuwe)
```

The installed configuration only looks for dependencies that were linked into
that particular Wuwe build. Missing required dependencies fail during
`find_package(wuwe)` with a direct diagnostic instead of surfacing later as an
unresolved imported target.
