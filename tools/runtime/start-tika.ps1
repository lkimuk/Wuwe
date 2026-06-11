param(
  [string] $HostName = "127.0.0.1",
  [int] $Port = 9998,
  [string] $JavaPath = "",
  [string] $TikaJar = ""
)

$ErrorActionPreference = "Stop"

$ScriptRoot = $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($TikaJar)) {
  $TikaJar = Get-ChildItem -LiteralPath $ScriptRoot -Filter "tika-server*.jar" |
    Select-Object -First 1 -ExpandProperty FullName
}

if ([string]::IsNullOrWhiteSpace($TikaJar) -or -not (Test-Path -LiteralPath $TikaJar)) {
  throw "Tika jar was not found next to this script. Pass -TikaJar explicitly."
}

if ([string]::IsNullOrWhiteSpace($JavaPath)) {
  $BundledJava = Join-Path $ScriptRoot "..\jre\bin\java.exe"
  if (Test-Path -LiteralPath $BundledJava) {
    $JavaPath = $BundledJava
  }
  else {
    $JavaPath = "java"
  }
}

& $JavaPath -jar $TikaJar --host $HostName --port $Port
if ($LASTEXITCODE -ne 0) {
  throw "Tika exited with code $LASTEXITCODE"
}
