param(
  [string] $ServerPath = "build\examples\Debug\mcp_stdio_example.exe",
  [switch] $Build
)

$ErrorActionPreference = "Stop"

$arguments = @("tools/mcp-host-transcript.mjs", "--server", $ServerPath)
if ($Build) {
  $arguments += "--build"
}

node @arguments
exit $LASTEXITCODE
