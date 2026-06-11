param(
  [ValidateSet("Debug", "Release")]
  [string] $Configuration = "Release",
  [string] $BuildDir = "build",
  [string] $ArtifactsDir = "artifacts",
  [string] $DistDir = "dist",
  [string] $Platform = "windows-x64",
  [string] $TikaJar = "third_party\runtime\tika\tika-server-standard.jar",
  [string] $JreArchive = "third_party\runtime\jre\temurin-21-jre-windows-x64.zip",
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

function Copy-JreRuntime {
  param(
    [Parameter(Mandatory = $true)][string] $Destination,
    [string] $SourceDir = "",
    [string] $SourceArchive = "",
    [switch] $Required
  )

  Remove-DirectoryIfExists $Destination

  if (-not [string]::IsNullOrWhiteSpace($SourceDir)) {
    $ResolvedSourceDir = Resolve-RepoPath $SourceDir
    if (-not (Test-Path -LiteralPath $ResolvedSourceDir)) {
      throw "JRE directory not found: $ResolvedSourceDir"
    }
    Copy-DirectoryContents $ResolvedSourceDir $Destination
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

  $TempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("wuwe-jre-" + [System.Guid]::NewGuid().ToString("N"))
  New-Item -ItemType Directory -Force -Path $TempRoot | Out-Null
  try {
    Expand-Archive -LiteralPath $ResolvedArchive -DestinationPath $TempRoot -Force
    $children = @(Get-ChildItem -LiteralPath $TempRoot)
    $jreRoot = $TempRoot
    if ($children.Count -eq 1 -and $children[0].PSIsContainer) {
      $jreRoot = $children[0].FullName
    }

    $javaExe = Join-Path $jreRoot "bin\java.exe"
    if (-not (Test-Path -LiteralPath $javaExe)) {
      throw "JRE archive does not contain bin\java.exe: $ResolvedArchive"
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

function New-PackageManifest {
  param(
    [Parameter(Mandatory = $true)][string] $Name,
    [Parameter(Mandatory = $true)][string] $Root,
    [Parameter(Mandatory = $true)][string] $Version,
    [Parameter(Mandatory = $true)][string] $Configuration,
    [Parameter(Mandatory = $true)][string] $Platform,
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
    return
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

$BuildPath = Join-Path $RepoRoot $BuildDir
$ArtifactsPath = Join-Path $RepoRoot $ArtifactsDir
$DistPath = Join-Path $RepoRoot $DistDir
$PackageRoot = Join-Path $ArtifactsPath "wuwe"

New-Item -ItemType Directory -Force -Path $ArtifactsPath, $DistPath | Out-Null

if (-not $SkipBuild) {
  Invoke-Native cmake @("--build", $BuildPath, "--config", $Configuration)
}

if (-not $KeepArtifacts) {
  Remove-DirectoryIfExists $PackageRoot
}

$TikaJarPath = Resolve-RepoPath $TikaJar
if (-not (Test-Path -LiteralPath $TikaJarPath)) {
  throw "Tika jar not found: $TikaJarPath. Place the pinned project jar at third_party\runtime\tika\tika-server-standard.jar."
}
Assert-TikaJarChecksum $TikaJarPath

Invoke-Native cmake @("--install", $BuildPath, "--config", $Configuration, "--prefix", $PackageRoot)
Copy-IfExists (Join-Path $RepoRoot "README.md") $PackageRoot
Copy-IfExists (Join-Path $RepoRoot "LICENSE") $PackageRoot
Copy-IfExists (Join-Path $RepoRoot "VERSION") $PackageRoot
Copy-IfExists (Join-Path $RepoRoot "docs") (Join-Path $PackageRoot "docs")
Copy-IfExists (Join-Path $RepoRoot "examples") (Join-Path $PackageRoot "examples")

$runtimeTika = Join-Path $PackageRoot "runtime\tika"
New-Item -ItemType Directory -Force -Path $runtimeTika | Out-Null
Copy-Item -LiteralPath $TikaJarPath -Destination $runtimeTika -Force
Copy-IfExists "$TikaJarPath.sha1" $runtimeTika
Copy-IfExists (Join-Path (Split-Path -Parent $TikaJarPath) "README.md") $runtimeTika
Copy-Item -LiteralPath (Join-Path $RepoRoot "tools\runtime\start-tika.ps1") -Destination $runtimeTika -Force
Copy-Item -LiteralPath (Join-Path $RepoRoot "tools\runtime\start-tika.sh") -Destination $runtimeTika -Force

$runtime = [ordered] @{
  tika = [ordered] @{
    jar = "runtime/tika/" + (Split-Path -Leaf $TikaJarPath)
    sha256 = (Get-FileHash -LiteralPath $TikaJarPath -Algorithm SHA256).Hash.ToLowerInvariant()
    internal_url = "http://127.0.0.1:9998"
  }
}

$jreRequired = $Platform -eq "windows-x64"
$jreRuntime = Copy-JreRuntime `
  -Destination (Join-Path $PackageRoot "runtime\jre") `
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
  -Runtime $runtime
Write-Checksums $PackageRoot

$zip = Join-Path $DistPath "wuwe-$Version-$Platform.zip"
New-ZipFromDirectory $PackageRoot $zip
Write-Host "Created $zip"
