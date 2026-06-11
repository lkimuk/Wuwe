# Packaging

Wuwe ships as one complete package: `wuwe`.

The package includes the C++ SDK, examples, documentation, and the runtime
sidecars needed for the default RAG experience. Users should not need to know
that PDF and Office parsing is backed by Apache Tika or Java.

## Package Contents

`wuwe` contains:

- installed headers,
- installed libraries,
- CMake package files,
- repository docs,
- example source files,
- `README.md`, `LICENSE`, `VERSION`,
- `manifest.json`,
- `checksums.sha256`,
- bundled runtime sidecars under `runtime/`.

The Windows x64 package includes:

- `runtime/tika/tika-server-standard.jar`,
- `runtime/jre/bin/java.exe`,
- runtime metadata in `manifest.json`.

## Build Package

Generate the package:

```powershell
cmake -S . -B build
cmake --build build --config Release
.\tools\package-wuwe.ps1 -Configuration Release
```

Install directly to a prefix:

```powershell
cmake --install build --config Release --prefix install
```

The install prefix is also self-contained: it includes the SDK files and bundled
runtime sidecars under `runtime/`. This supports applications such as ReArk that
consume Wuwe directly from an install directory during development or installer
staging.

By default, packaging uses the pinned project artifacts at:

```text
third_party/runtime/tika/tika-server-standard.jar
third_party/runtime/jre/temurin-21-jre-windows-x64.zip
```

The script does not download Tika or Java during packaging. The project carries
the pinned Tika Server jar, Maven checksum file, JRE archive, and JRE SHA-256
file so releases can be generated without extra parameters. When checksum files
are present, the package script validates them before producing the archive.

Release engineers may pass `-JreDir` or `-JreArchive` when intentionally
replacing the bundled JRE for a platform build.

## Runtime Behavior

Applications normally use:

```cpp
auto loader = wuwe::agent::knowledge::knowledge_document_loader::make_default();
```

When the package layout contains `runtime/tika`, the loader starts the bundled
parser automatically and keeps it alive for the loader lifetime. The runtime
uses `runtime/jre/bin/java.exe` in Windows x64 release packages. Developer
installs that explicitly disable bundled runtimes may still run a manually
managed parser, but the default release path is self-contained.

There is no user-facing Tika configuration in the default path.

## Release Checks

Before publishing:

- build Release,
- run tests,
- run package smoke tests,
- generate `wuwe`,
- inspect `manifest.json`,
- verify `checksums.sha256`,
- verify the pinned Tika jar version and checksum under `third_party/runtime/tika`,
- verify the pinned JRE version and checksum under `third_party/runtime/jre`,
- include third-party license and notice files required by bundled runtimes.

The package script records the Tika jar and JRE archive SHA-256 values in the
manifest, but release ownership still needs a pinned dependency policy outside
the script.
