param(
  [ValidateSet("Debug", "Release")]
  [string] $Configuration = "Debug",
  [string] $BuildDir = "build",
  [string] $InstallDir = "build\install-check",
  [string] $PackageSmokeDir = "build\package-smoke",
  [string] $VcpkgToolchain = "D:\tools\vcpkg\scripts\buildsystems\vcpkg.cmake",
  [switch] $SkipHostTranscript
)

$ErrorActionPreference = "Stop"

function Invoke-Native {
  param(
    [Parameter(Mandatory = $true)]
    [string] $Command,
    [string[]] $Arguments = @()
  )

  & $Command @Arguments
  if ($LASTEXITCODE -ne 0) {
    throw "$Command failed with exit code $LASTEXITCODE"
  }
}

$RepoRoot = (Resolve-Path ".").Path
$ConfigName = $Configuration.ToLowerInvariant()
if ($InstallDir -eq "build\install-check") {
  $InstallDir = "build\install-check-$ConfigName"
}
if ($PackageSmokeDir -eq "build\package-smoke") {
  $PackageSmokeDir = "build\package-smoke-$ConfigName"
}

$InstallPath = Join-Path $RepoRoot $InstallDir
$PackageSmokePath = Join-Path $RepoRoot $PackageSmokeDir
if (Test-Path -LiteralPath $PackageSmokePath) {
  Remove-Item -LiteralPath $PackageSmokePath -Recurse -Force
}

Invoke-Native cmake @("--build", $BuildDir, "--config", $Configuration)
Invoke-Native (Join-Path $RepoRoot "$BuildDir\tests\$Configuration\mcp_tests.exe")
if (-not $SkipHostTranscript) {
  Invoke-Native node @(
    "tools/mcp-host-transcript.mjs",
    "--server",
    (Join-Path $RepoRoot "$BuildDir\examples\$Configuration\mcp_stdio_example.exe")
  )
}
Invoke-Native ctest @("--test-dir", $BuildDir, "-C", $Configuration, "--output-on-failure")
Invoke-Native cmake @("--install", $BuildDir, "--config", $Configuration, "--prefix", $InstallPath)
Invoke-Native cmake @(
  "-S", "tests\package_smoke",
  "-B", $PackageSmokePath,
  "-DCMAKE_PREFIX_PATH=$InstallPath",
  "-DCMAKE_TOOLCHAIN_FILE=$VcpkgToolchain",
  "-DCMAKE_CONFIGURATION_TYPES=$Configuration"
)
Invoke-Native cmake @("--build", $PackageSmokePath, "--config", $Configuration)
Invoke-Native (Join-Path $PackageSmokePath "$Configuration\wuwe_package_smoke.exe")
