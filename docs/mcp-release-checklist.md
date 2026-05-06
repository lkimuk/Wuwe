# MCP Release Checklist

Run this checklist before publishing MCP-facing changes.

## One-Command Validation

```powershell
.\tools\mcp-release-check.ps1 -Configuration Debug
.\tools\mcp-release-check.ps1 -Configuration Release
```

The script performs a full build for the selected configuration, runs
`mcp_tests`, runs the host process transcript, runs `ctest`, installs the
package to an isolated `build\install-check-<config>` directory, configures a
clean package smoke build, and runs `wuwe_package_smoke.exe`.

## Host Transcript Validation

```powershell
.\tools\mcp-host-transcript.ps1
.\tools\mcp-host-transcript.ps1 -ServerPath build\examples\Release\mcp_stdio_example.exe
```

This validates the example through real process stdio instead of in-memory
streams. Run it before claiming desktop host compatibility, especially on
Windows where stdout text mode can corrupt MCP CRLF framing.

Use `.\tools\mcp-release-check.ps1 -SkipHostTranscript` only when the local
sandbox cannot spawn child processes.

## Manual Debug Validation

```powershell
cmake --build build --config Debug
.\build\tests\Debug\mcp_tests.exe
ctest --test-dir build -C Debug --output-on-failure
cmake --install build --config Debug --prefix build\install-check-debug
cmake -S tests\package_smoke -B build\package-smoke-debug `
  -DCMAKE_PREFIX_PATH="$PWD\build\install-check-debug" `
  -DCMAKE_TOOLCHAIN_FILE=D:\tools\vcpkg\scripts\buildsystems\vcpkg.cmake `
  -DCMAKE_CONFIGURATION_TYPES=Debug
cmake --build build\package-smoke-debug --config Debug
.\build\package-smoke-debug\Debug\wuwe_package_smoke.exe
```

## Manual Release Validation

```powershell
cmake --build build --config Release
.\build\tests\Release\mcp_tests.exe
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix build\install-check-release
cmake -S tests\package_smoke -B build\package-smoke-release `
  -DCMAKE_PREFIX_PATH="$PWD\build\install-check-release" `
  -DCMAKE_TOOLCHAIN_FILE=D:\tools\vcpkg\scripts\buildsystems\vcpkg.cmake `
  -DCMAKE_CONFIGURATION_TYPES=Release
cmake --build build\package-smoke-release --config Release
.\build\package-smoke-release\Release\wuwe_package_smoke.exe
```

## Host Validation

Record real host results in [MCP Host Compatibility](mcp-host-compatibility.md):

- Host name and version.
- Exact executable path used.
- Tools/resources/prompts/roots visibility.
- Whether sampling and elicitation are fulfilled or reported unsupported.
- Any protocol errors or host-specific limitations.
