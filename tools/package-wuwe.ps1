param(
  [ValidateSet("Debug", "Release")]
  [string] $Configuration = "Release",
  [string] $BuildDir = "build",
  [string] $ArtifactsDir = "artifacts",
  [string] $DistDir = "dist",
  [ValidateSet("windows-x64", "linux-x64")]
  [string] $Platform = "windows-x64",
  [string] $TikaJar = "third_party/runtime/tika/tika-server-standard.jar",
  [string] $JreArchive = "",
  [string] $JreDir = "",
  [switch] $SkipBuild,
  [switch] $KeepArtifacts
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

function Remove-DirectoryIfExists {
  param([Parameter(Mandatory = $true)][string] $Path)
  if (Test-Path -LiteralPath $Path) {
    Remove-Item -LiteralPath $Path -Recurse -Force
  }
}

function Copy-DirectoryContents {
  param(
    [Parameter(Mandatory = $true)][string] $Source,
    [Parameter(Mandatory = $true)][string] $Destination
  )

  New-Item -ItemType Directory -Force -Path $Destination | Out-Null
  Copy-Item -Path (Join-Path $Source "*") -Destination $Destination -Recurse -Force
}

function Copy-IfExists {
  param(
    [Parameter(Mandatory = $true)][string] $Source,
    [Parameter(Mandatory = $true)][string] $Destination
  )

  if (Test-Path -LiteralPath $Source) {
    Copy-Item -LiteralPath $Source -Destination $Destination -Recurse -Force
  }
}

function Get-DefaultJreArchive {
  param([Parameter(Mandatory = $true)][string] $TargetPlatform)

  switch ($TargetPlatform.ToLowerInvariant()) {
    "windows-x64" { return "third_party/runtime/jre/temurin-21-jre-windows-x64.zip" }
    "linux-x64" { return "third_party/runtime/jre/temurin-21-jre-linux-x64.tar.gz" }
    default { return "" }
  }
}

function Get-JavaRelativePath {
  param([Parameter(Mandatory = $true)][string] $TargetPlatform)

  switch ($TargetPlatform.ToLowerInvariant()) {
    "windows-x64" { return "bin/java.exe" }
    "linux-x64" { return "bin/java" }
    default { return "bin/java" }
  }
}

function Assert-PackageHostMatchesPlatform {
  param([Parameter(Mandatory = $true)][string] $TargetPlatform)

  $hostPlatform = if ([System.Runtime.InteropServices.RuntimeInformation]::IsOSPlatform(
      [System.Runtime.InteropServices.OSPlatform]::Windows)) {
    "windows"
  }
  elseif ([System.Runtime.InteropServices.RuntimeInformation]::IsOSPlatform(
      [System.Runtime.InteropServices.OSPlatform]::Linux)) {
    "linux"
  }
  else {
    "unsupported"
  }

  $requiredHost = if ($TargetPlatform.StartsWith("windows-")) { "windows" } else { "linux" }
  if ($hostPlatform -ne $requiredHost) {
    throw "Package platform $TargetPlatform must be generated on a $requiredHost host; current host is $hostPlatform."
  }
}

function Copy-JreRuntime {
  param(
    [Parameter(Mandatory = $true)][string] $Destination,
    [Parameter(Mandatory = $true)][string] $TargetPlatform,
    [string] $SourceDir = "",
    [string] $SourceArchive = "",
    [switch] $Required
  )

  Remove-DirectoryIfExists $Destination
  $javaRelativePath = Get-JavaRelativePath $TargetPlatform

  if (-not [string]::IsNullOrWhiteSpace($SourceDir)) {
    $ResolvedSourceDir = Resolve-RepoPath $SourceDir
    if (-not (Test-Path -LiteralPath $ResolvedSourceDir)) {
      throw "JRE directory not found: $ResolvedSourceDir"
    }
    if (-not (Test-Path -LiteralPath (Join-Path $ResolvedSourceDir $javaRelativePath))) {
      throw "JRE directory does not contain ${javaRelativePath}: $ResolvedSourceDir"
    }
    if ($TargetPlatform -eq "linux-x64") {
      New-Item -ItemType Directory -Force -Path $Destination | Out-Null
      Invoke-Native cp @("-a", "$ResolvedSourceDir/.", $Destination)
    }
    else {
      Copy-DirectoryContents $ResolvedSourceDir $Destination
    }
    return [ordered] @{
      bundled = $true
      path = "runtime/jre"
      source = "directory"
    }
  }

  if ([string]::IsNullOrWhiteSpace($SourceArchive)) {
    if ($Required) {
      throw "JRE archive is required for this package but no archive path was provided."
    }
    return [ordered] @{ bundled = $false }
  }

  $ResolvedArchive = Resolve-RepoPath $SourceArchive
  if (-not (Test-Path -LiteralPath $ResolvedArchive)) {
    if ($Required) {
      throw "JRE archive not found: $ResolvedArchive"
    }
    return [ordered] @{ bundled = $false }
  }
  Assert-Sha256Checksum $ResolvedArchive

  $archiveName = $ResolvedArchive.ToLowerInvariant()
  if ($TargetPlatform -eq "linux-x64" -and
      ($archiveName.EndsWith(".tar.gz") -or $archiveName.EndsWith(".tgz"))) {
    $entries = @(& tar -tzf $ResolvedArchive)
    if ($LASTEXITCODE -ne 0 -or $entries.Count -eq 0) {
      throw "Unable to inspect JRE archive: $ResolvedArchive"
    }

    $topLevelEntries = @($entries |
      ForEach-Object { $_.TrimEnd("/").Split("/")[0] } |
      Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
      Sort-Object -Unique)

    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    $tarArguments = @("-xzf", $ResolvedArchive, "-C", $Destination)
    if ($topLevelEntries.Count -eq 1) {
      $tarArguments += "--strip-components=1"
    }
    Invoke-Native tar $tarArguments

    if (-not (Test-Path -LiteralPath (Join-Path $Destination $javaRelativePath))) {
      throw "JRE archive does not contain ${javaRelativePath}: $ResolvedArchive"
    }
    return [ordered] @{
      bundled = $true
      path = "runtime/jre"
      source = "archive"
      archive = [System.IO.Path]::GetRelativePath($RepoRoot, $ResolvedArchive).Replace("\", "/")
      sha256 = (Get-FileHash -LiteralPath $ResolvedArchive -Algorithm SHA256).Hash.ToLowerInvariant()
    }
  }

  $TempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("wuwe-jre-" + [System.Guid]::NewGuid().ToString("N"))
  New-Item -ItemType Directory -Force -Path $TempRoot | Out-Null
  try {
    if ($archiveName.EndsWith(".zip")) {
      Expand-Archive -LiteralPath $ResolvedArchive -DestinationPath $TempRoot -Force
    }
    elseif ($archiveName.EndsWith(".tar.gz") -or $archiveName.EndsWith(".tgz")) {
      Invoke-Native tar @("-xzf", $ResolvedArchive, "-C", $TempRoot)
    }
    elseif ($archiveName.EndsWith(".tar")) {
      Invoke-Native tar @("-xf", $ResolvedArchive, "-C", $TempRoot)
    }
    else {
      throw "Unsupported JRE archive format: $ResolvedArchive"
    }

    $children = @(Get-ChildItem -LiteralPath $TempRoot)
    $jreRoot = $TempRoot
    if ($children.Count -eq 1 -and $children[0].PSIsContainer) {
      $jreRoot = $children[0].FullName
    }

    $javaExe = Join-Path $jreRoot $javaRelativePath
    if (-not (Test-Path -LiteralPath $javaExe)) {
      throw "JRE archive does not contain ${javaRelativePath}: $ResolvedArchive"
    }

    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    Copy-Item -Path (Join-Path $jreRoot "*") -Destination $Destination -Recurse -Force
    return [ordered] @{
      bundled = $true
      path = "runtime/jre"
      source = "archive"
      archive = [System.IO.Path]::GetRelativePath($RepoRoot, $ResolvedArchive).Replace("\", "/")
      sha256 = (Get-FileHash -LiteralPath $ResolvedArchive -Algorithm SHA256).Hash.ToLowerInvariant()
    }
  }
  finally {
    Remove-DirectoryIfExists $TempRoot
  }
}

function Get-CMakeCacheValue {
  param(
    [Parameter(Mandatory = $true)][string] $CachePath,
    [Parameter(Mandatory = $true)][string] $Name,
    [string] $Default = ""
  )

  if (-not (Test-Path -LiteralPath $CachePath)) {
    throw "CMake cache not found: $CachePath"
  }

  $prefix = "$Name`:"
  $line = Get-Content -LiteralPath $CachePath |
    Where-Object { $_.StartsWith($prefix, [System.StringComparison]::Ordinal) } |
    Select-Object -First 1
  if ($null -eq $line) {
    return $Default
  }

  $separator = $line.IndexOf("=")
  if ($separator -lt 0) {
    return $Default
  }
  return $line.Substring($separator + 1)
}

function ConvertFrom-CMakeBool {
  param([string] $Value)
  return @("1", "ON", "TRUE", "YES", "Y") -contains $Value.ToUpperInvariant()
}

function Get-VcpkgPackageVersion {
  param(
    [Parameter(Mandatory = $true)][string] $StatusPath,
    [Parameter(Mandatory = $true)][string] $PackageName
  )

  if (-not (Test-Path -LiteralPath $StatusPath)) {
    return "unknown"
  }

  $content = Get-Content -LiteralPath $StatusPath -Raw
  $escapedName = [regex]::Escape($PackageName)
  $match = [regex]::Match(
    $content,
    "(?ms)^Package: $escapedName\r?\n.*?^Version: ([^\r\n]+)(?:\r?\nPort-Version: ([^\r\n]+))?")
  if (-not $match.Success) {
    return "unknown"
  }

  $version = $match.Groups[1].Value
  $portVersion = $match.Groups[2].Value
  if (-not [string]::IsNullOrWhiteSpace($portVersion) -and $portVersion -ne "0") {
    return "$version#$portVersion"
  }
  return $version
}

function New-PackageManifest {
  param(
    [Parameter(Mandatory = $true)][string] $Name,
    [Parameter(Mandatory = $true)][string] $Root,
    [Parameter(Mandatory = $true)][string] $Version,
    [Parameter(Mandatory = $true)][string] $Configuration,
    [Parameter(Mandatory = $true)][string] $Platform,
    [object] $Capabilities = @{},
    [object] $BuildDependencies = @{},
    [object] $Runtime = @{}
  )

  $manifest = [ordered] @{
    name = $Name
    version = $Version
    configuration = $Configuration
    platform = $Platform
    generated_at_utc = (Get-Date).ToUniversalTime().ToString("o")
    layout = [ordered] @{
      include = "C++ headers"
      lib = "C++ libraries and CMake package files"
      docs = "documentation copied from repository docs"
      examples = "example source files when present"
      runtime = "bundled runtime sidecars"
    }
    capabilities = $Capabilities
    build_dependencies = $BuildDependencies
    runtime = $Runtime
  }

  $manifestPath = Join-Path $Root "manifest.json"
  $manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath -Encoding UTF8
}

function Write-Checksums {
  param(
    [Parameter(Mandatory = $true)][string] $Root
  )

  $checksumPath = Join-Path $Root "checksums.sha256"
  $files = Get-ChildItem -LiteralPath $Root -Recurse -File |
    Where-Object { $_.FullName -ne $checksumPath } |
    Sort-Object FullName

  $lines = foreach ($file in $files) {
    $hash = Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256
    $relative = [System.IO.Path]::GetRelativePath($Root, $file.FullName).Replace("\", "/")
    "$($hash.Hash.ToLowerInvariant())  $relative"
  }

  $lines | Set-Content -LiteralPath $checksumPath -Encoding ASCII
}

function New-ZipFromDirectory {
  param(
    [Parameter(Mandatory = $true)][string] $Root,
    [Parameter(Mandatory = $true)][string] $ZipPath
  )

  if (Test-Path -LiteralPath $ZipPath) {
    Remove-Item -LiteralPath $ZipPath -Force
  }

  $items = Get-ChildItem -LiteralPath $Root
  Compress-Archive -Path $items.FullName -DestinationPath $ZipPath -Force
}

function New-TarGzFromDirectory {
  param(
    [Parameter(Mandatory = $true)][string] $Root,
    [Parameter(Mandatory = $true)][string] $ArchivePath
  )

  if (Test-Path -LiteralPath $ArchivePath) {
    Remove-Item -LiteralPath $ArchivePath -Force
  }
  Invoke-Native tar @("-czf", $ArchivePath, "-C", $Root, ".")
}

function Assert-TikaJarChecksum {
  param([Parameter(Mandatory = $true)][string] $JarPath)

  $sha1Path = "$JarPath.sha1"
  if (-not (Test-Path -LiteralPath $sha1Path)) {
    return
  }

  $expected = (Get-Content -LiteralPath $sha1Path -Raw).Trim().ToLowerInvariant()
  $actual = (Get-FileHash -LiteralPath $JarPath -Algorithm SHA1).Hash.ToLowerInvariant()
  if ($expected -ne $actual) {
    throw "Tika jar SHA-1 mismatch. Expected $expected but found $actual for $JarPath."
  }
}

function Assert-Sha256Checksum {
  param([Parameter(Mandatory = $true)][string] $Path)

  $sha256Path = "$Path.sha256"
  if (-not (Test-Path -LiteralPath $sha256Path)) {
    throw "SHA-256 file not found: $sha256Path"
  }

  $expectedLine = (Get-Content -LiteralPath $sha256Path -Raw).Trim().Split([Environment]::NewLine)[0]
  $expected = ($expectedLine -split "\s+")[0].ToLowerInvariant()
  $actual = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
  if ($expected -ne $actual) {
    throw "SHA-256 mismatch. Expected $expected but found $actual for $Path."
  }
}

function Resolve-RepoPath {
  param([Parameter(Mandatory = $true)][string] $Path)
  if ([System.IO.Path]::IsPathRooted($Path)) {
    return $Path
  }
  return (Join-Path $RepoRoot $Path)
}

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$Version = (Get-Content -LiteralPath (Join-Path $RepoRoot "VERSION")).Trim()
Assert-PackageHostMatchesPlatform $Platform

if ([string]::IsNullOrWhiteSpace($JreArchive) -and
    [string]::IsNullOrWhiteSpace($JreDir)) {
  $JreArchive = Get-DefaultJreArchive $Platform
}

$BuildPath = Join-Path $RepoRoot $BuildDir
$ArtifactsPath = Join-Path $RepoRoot $ArtifactsDir
$DistPath = Join-Path $RepoRoot $DistDir
$PackageRoot = Join-Path $ArtifactsPath "wuwe"
$CMakeCachePath = Join-Path $BuildPath "CMakeCache.txt"

New-Item -ItemType Directory -Force -Path $ArtifactsPath, $DistPath | Out-Null

if (-not $SkipBuild) {
  Invoke-Native cmake @("--build", $BuildPath, "--config", $Configuration)
}

$withOpenSsl = ConvertFrom-CMakeBool (Get-CMakeCacheValue $CMakeCachePath "WUWE_BUILT_WITH_OPENSSL" "OFF")
$withHttplibHttps = ConvertFrom-CMakeBool (Get-CMakeCacheValue $CMakeCachePath "WUWE_BUILT_WITH_HTTPLIB_HTTPS" "OFF")
$withSqlite = ConvertFrom-CMakeBool (Get-CMakeCacheValue $CMakeCachePath "WUWE_BUILT_WITH_SQLITE" "OFF")
$sqliteProvider = Get-CMakeCacheValue $CMakeCachePath "WUWE_SQLITE_DEPENDENCY" "none"
$vcpkgStatusPath = Join-Path $BuildPath "vcpkg_installed/vcpkg/status"
$vcpkgManifestPath = Join-Path $RepoRoot "vcpkg.json"
$vcpkgBaseline = "unknown"
if (Test-Path -LiteralPath $vcpkgManifestPath) {
  $vcpkgManifest = Get-Content -LiteralPath $vcpkgManifestPath -Raw | ConvertFrom-Json
  $vcpkgBaseline = $vcpkgManifest.'builtin-baseline'
}
$capabilities = [ordered] @{
  http_backend = Get-CMakeCacheValue $CMakeCachePath "WUWE_HTTP_BACKEND" "cpr"
  tls_backend = Get-CMakeCacheValue $CMakeCachePath "WUWE_TLS_BACKEND_RESOLVED" "native"
  openssl_linked = $withOpenSsl
  httplib_https = $withHttplibHttps
  sqlite_memory_store = $withSqlite
  sqlite_knowledge_index = $withSqlite
  sqlite_knowledge_search = if ($withSqlite) { "persistent-linear-scan" } else { "disabled" }
}
$buildDependencies = [ordered] @{
  vcpkg_baseline = $vcpkgBaseline
  openssl = [ordered] @{
    linked = $withOpenSsl
    version = if ($withOpenSsl) { Get-VcpkgPackageVersion $vcpkgStatusPath "openssl" } else { "not-linked" }
  }
  sqlite3 = [ordered] @{
    linked = $withSqlite
    provider = if ($withSqlite) { $sqliteProvider } else { "not-linked" }
    version = if ($withSqlite) { Get-VcpkgPackageVersion $vcpkgStatusPath "sqlite3" } else { "not-linked" }
  }
  cpr_libcurl = [ordered] @{
    source = "pinned-fetchcontent"
    included_cmake_package = $true
  }
}

if (-not $KeepArtifacts) {
  Remove-DirectoryIfExists $PackageRoot
}

$TikaJarPath = Resolve-RepoPath $TikaJar
if (-not (Test-Path -LiteralPath $TikaJarPath)) {
  throw "Tika jar not found: $TikaJarPath. Place the pinned project jar at third_party/runtime/tika/tika-server-standard.jar."
}
Assert-TikaJarChecksum $TikaJarPath

Invoke-Native cmake @("--install", $BuildPath, "--config", $Configuration, "--prefix", $PackageRoot)
Copy-IfExists (Join-Path $RepoRoot "README.md") $PackageRoot
Copy-IfExists (Join-Path $RepoRoot "LICENSE") $PackageRoot
Copy-IfExists (Join-Path $RepoRoot "VERSION") $PackageRoot
Copy-IfExists (Join-Path $RepoRoot "vcpkg.json") $PackageRoot
Copy-IfExists (Join-Path $RepoRoot "docs") (Join-Path $PackageRoot "docs")
Copy-IfExists (Join-Path $RepoRoot "examples") (Join-Path $PackageRoot "examples")

$runtimeTika = Join-Path $PackageRoot "runtime/tika"
New-Item -ItemType Directory -Force -Path $runtimeTika | Out-Null
Copy-Item -LiteralPath $TikaJarPath -Destination $runtimeTika -Force
Copy-IfExists "$TikaJarPath.sha1" $runtimeTika
Copy-IfExists (Join-Path (Split-Path -Parent $TikaJarPath) "README.md") $runtimeTika
Copy-Item -LiteralPath (Join-Path $RepoRoot "tools/runtime/start-tika.ps1") -Destination $runtimeTika -Force
Copy-Item -LiteralPath (Join-Path $RepoRoot "tools/runtime/start-tika.sh") -Destination $runtimeTika -Force

$runtime = [ordered] @{
  tika = [ordered] @{
    jar = "runtime/tika/" + (Split-Path -Leaf $TikaJarPath)
    sha256 = (Get-FileHash -LiteralPath $TikaJarPath -Algorithm SHA256).Hash.ToLowerInvariant()
    internal_url = "http://127.0.0.1:9998"
  }
}

$jreRequired = @("windows-x64", "linux-x64") -contains $Platform.ToLowerInvariant()
$jreRuntime = Copy-JreRuntime `
  -Destination (Join-Path $PackageRoot "runtime/jre") `
  -TargetPlatform $Platform `
  -SourceDir $JreDir `
  -SourceArchive $JreArchive `
  -Required:$jreRequired
if ($jreRuntime.bundled) {
  $runtime.jre = $jreRuntime
}
else {
  $runtime.jre = [ordered] @{
    bundled = $false
    note = "runtime will use java from PATH when runtime/jre is absent"
  }
}

New-PackageManifest `
  -Name "wuwe" `
  -Root $PackageRoot `
  -Version $Version `
  -Configuration $Configuration `
  -Platform $Platform `
  -Capabilities $capabilities `
  -BuildDependencies $buildDependencies `
  -Runtime $runtime
Write-Checksums $PackageRoot

if ($Platform -eq "linux-x64") {
  $archive = Join-Path $DistPath "wuwe-$Version-$Platform.tar.gz"
  New-TarGzFromDirectory $PackageRoot $archive
}
else {
  $archive = Join-Path $DistPath "wuwe-$Version-$Platform.zip"
  New-ZipFromDirectory $PackageRoot $archive
}
Write-Host "Created $archive"
