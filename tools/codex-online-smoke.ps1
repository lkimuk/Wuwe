param(
  [string]$AccessToken = $env:WUWE_CODEX_ACCESS_TOKEN,
  [string]$AccountId = $env:WUWE_CODEX_ACCOUNT_ID,
  [string]$Model = $env:WUWE_CODEX_MODEL,
  [string]$ClientVersion = $(if ($env:WUWE_CODEX_CLIENT_VERSION) { $env:WUWE_CODEX_CLIENT_VERSION } else { '0.153.4' }),
  [string]$Originator = $(if ($env:WUWE_CODEX_ORIGINATOR) { $env:WUWE_CODEX_ORIGINATOR } else { 'codex_cli_rs' })
)
$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($AccessToken) -or [string]::IsNullOrWhiteSpace($AccountId) -or [string]::IsNullOrWhiteSpace($Model)) {
  throw 'Set WUWE_CODEX_ACCESS_TOKEN, WUWE_CODEX_ACCOUNT_ID and WUWE_CODEX_MODEL.'
}
$headers = @{
  Authorization = "Bearer $AccessToken"
  'chatgpt-account-id' = $AccountId
  originator = $Originator
  version = $ClientVersion
  Accept = 'application/json'
}
try {
  $catalog = Invoke-WebRequest -UseBasicParsing -Headers $headers -Uri "https://chatgpt.com/backend-api/codex/models?client_version=$([uri]::EscapeDataString($ClientVersion))" -TimeoutSec 30
} catch {
  $status = if ($_.Exception.Response) { [int]$_.Exception.Response.StatusCode } else { 0 }
  throw "Model catalog request failed (HTTP $status)."
}
if ($catalog.StatusCode -ne 200) { throw "Model catalog returned HTTP $($catalog.StatusCode)." }
$catalogJson = $catalog.Content | ConvertFrom-Json
$models = @($catalogJson.models)
if ($models.Count -eq 0 -or -not ($models.slug -contains $Model)) { throw "Configured model is not present in the Codex catalog." }
$body = @{
  model = $Model; instructions = 'Reply with exactly: Wuwe online smoke passed.'
  input = @(@{ type = 'message'; role = 'user'; content = @(@{ type = 'input_text'; text = 'Online smoke check.' }) })
  tools = @(); tool_choice = 'none'; parallel_tool_calls = $true
  store = $false; stream = $true; include = @('reasoning.encrypted_content')
} | ConvertTo-Json -Depth 8 -Compress
$streamHeaders = $headers.Clone(); $streamHeaders.Accept = 'text/event-stream'; $streamHeaders.'Content-Type' = 'application/json'
try {
  $response = Invoke-WebRequest -UseBasicParsing -Method Post -Headers $streamHeaders -Body $body -Uri 'https://chatgpt.com/backend-api/codex/responses' -TimeoutSec 120
} catch {
  $status = if ($_.Exception.Response) { [int]$_.Exception.Response.StatusCode } else { 0 }
  throw "Codex generation request failed (HTTP $status)."
}
if ($response.StatusCode -ne 200) { throw "Codex generation returned HTTP $($response.StatusCode)." }
$sawCompleted = $false; $text = [System.Text.StringBuilder]::new()
foreach ($line in ($response.Content -split "`n")) {
  if ($line.StartsWith('data: ')) {
    $event = $line.Substring(6).Trim()
    if ($event -eq '[DONE]') { continue }
    try { $json = $event | ConvertFrom-Json } catch { continue }
    if ($json.type -eq 'response.output_text.delta') { [void]$text.Append($json.delta) }
    if ($json.type -eq 'response.completed') { $sawCompleted = $true }
  }
}
if (-not $sawCompleted) { throw 'SSE stream did not contain response.completed.' }
Write-Output ("Codex online smoke passed: catalog={0}, model={1}, text_bytes={2}" -f $models.Count, $Model, $text.Length)
