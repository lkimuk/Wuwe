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
- `vcpkg.json` with the pinned source-build dependency baseline,
- `manifest.json`,
- `checksums.sha256`,
- bundled runtime sidecars under `runtime/`.

The Windows x64 package includes:

- `runtime/tika/tika-server-standard.jar`,
- `runtime/jre/bin/java.exe`,
- runtime metadata in `manifest.json`.

Linux x64 package builds use the same Tika JAR with a separate Linux x64
Temurin JRE. JRE archives are never shared across operating systems.

## Build Package

Generate the package:

```powershell
$env:VCPKG_ROOT = "D:\tools\vcpkg"
cmake --preset windows-vcpkg
cmake --build --preset windows-vcpkg-release
ctest --preset windows-vcpkg-release
.\tools\package-wuwe.ps1 -BuildDir build-vcpkg -Configuration Release
```

On Linux x64:

```bash
export VCPKG_ROOT="$HOME/vcpkg"
cmake --preset linux-vcpkg
cmake --build --preset linux-vcpkg-release
ctest --preset linux-vcpkg-release
bash ./tools/package-wuwe.sh \
  --build-dir build-linux-vcpkg \
  --configuration Release
```

Install directly to a prefix:

```powershell
cmake --install build-vcpkg --config Release --prefix install
```

The install prefix includes the SDK files and bundled runtime sidecars under
`runtime/`. This supports applications such as ReArk that consume Wuwe directly
from an install directory during development or installer staging. Public SDK
link dependencies selected by the build, such as SQLite3, are resolved through
the exported CMake package rather than copied into the install prefix.

By default, packaging uses the pinned project artifacts at:

```text
third_party/runtime/tika/tika-server-standard.jar
third_party/runtime/jre/temurin-21-jre-windows-x64.zip
third_party/runtime/jre/temurin-21-jre-linux-x64.tar.gz
```

The script does not download Tika or Java during packaging. The project carries
the pinned Tika Server jar, Maven checksum file, platform-specific JRE archives,
and JRE SHA-256 files so releases can be generated without extra parameters.
The package script selects the matching JRE from `-Platform` and validates it
before producing the archive.

Packages must be generated on their target operating system. This preserves
native executables, Unix permissions, and JRE symbolic links. Windows produces
a `.zip`; Linux uses the native Bash entry point and produces a `.tar.gz`.

Release engineers may pass `-JreDir` or `-JreArchive` when intentionally
replacing the bundled JRE for a platform build. The replacement must contain
the Java executable expected by that platform.

| Package platform | JRE archive | Java executable |
| --- | --- | --- |
| `windows-x64` | `temurin-21-jre-windows-x64.zip` | `runtime/jre/bin/java.exe` |
| `linux-x64` | `temurin-21-jre-linux-x64.tar.gz` | `runtime/jre/bin/java` |

## Runtime Behavior

Applications normally use:

```cpp
auto loader = wuwe::agent::knowledge::knowledge_document_loader::make_default();
```

When the package layout contains `runtime/tika`, the loader starts the bundled
parser automatically and keeps it alive for the loader lifetime. The runtime
uses `runtime/jre/bin/java.exe` on Windows and `runtime/jre/bin/java` on Linux.
Developer installs that explicitly disable bundled runtimes may still run a
manually managed parser, but supported package builds are self-contained.

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

## SDK Link Dependencies

“Complete package” means the archive contains the Wuwe SDK, its exported CMake
metadata, and the default document-parsing sidecars. It does not mean every
optional C/C++ development dependency is copied into the archive.

`manifest.json` records the resolved HTTP/TLS and SQLite capabilities, vcpkg
baseline, and resolved SQLite/OpenSSL versions when available. The installed
`wuwe-config.cmake` only requests dependencies used by that build:

- the standard Windows x64 profile uses Schannel and does not require OpenSSL;
- SQLite-enabled builds require a compatible SQLite3 development package when
  a consuming application links the static SDK;
- OpenSSL-enabled variants require a compatible OpenSSL development package
  and runtime supplied by the consuming environment.

See [Dependencies](dependencies.md) for the supported build modes and complete
dependency inventory.
