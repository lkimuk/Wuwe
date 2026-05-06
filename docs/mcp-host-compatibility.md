# MCP Host Compatibility

This guide is for validating `mcp_stdio_example` with real MCP hosts. The
example uses Content-Length framed stdio and exposes:

- Tools: `echo_text`, `preview_image`
- Resources: `wuwe://example/readme`
- Resource templates: `wuwe://example/{name}`
- Roots: host-injected roots when configured by the embedding application
- Prompts: `echo_prompt`
- Sampling and elicitation: supported by the library as server-initiated
  requests; the sample server does not trigger them during the default smoke.

## Build

```powershell
cmake --build build --config Debug --target mcp_stdio_example mcp_tests
.\build\tests\Debug\mcp_tests.exe
```

The unit suite includes a framed stdio compatibility transcript that exercises
the same handshake and operations listed below. Real host validation is tracked
separately so compatibility claims stay tied to a concrete host/version.

For a host-like stdio transcript outside the unit test process, run:

```powershell
.\tools\mcp-host-transcript.ps1
.\tools\mcp-host-transcript.ps1 -ServerPath build\examples\Release\mcp_stdio_example.exe
```

The transcript starts the example process, sends Content-Length framed MCP
messages on stdin, parses framed stdout, and validates initialize, ping, tools,
rich image content, resources, and prompts. This catches OS-level stdio issues
that in-memory tests cannot see, including Windows text-mode CRLF translation.

## Validation Matrix

| Host | Version | Date | Status | Notes |
| --- | --- | --- | --- | --- |
| Local framed stdio transcript | repository test | 2026-05-06 | Pass | Covered by `mcp_tests`. Exercises initialize, tools, rich content, resources, prompts, and Content-Length framing. |
| Local process stdio transcript | Debug executable | 2026-05-06 | Pass | `tools/mcp-host-transcript.ps1` passed against `build\examples\Debug\mcp_stdio_example.exe`. |
| Local process stdio transcript | Release executable | 2026-05-06 | Pass | `tools/mcp-host-transcript.ps1 -ServerPath build\examples\Release\mcp_stdio_example.exe` passed. |
| VS Code | Installed, GUI not exercised | 2026-05-06 | Config prepared | Workspace config is available at `.vscode/mcp.json`; open VS Code and confirm the `wuwe` and `wuwe-rag` servers in the MCP UI. |
| Claude Desktop | Not run | Not run | Pending | Run the host config below and record the observed version. |
| Cursor | Not run | Not run | Pending | Run the host config below and record the observed version. |
| Continue | Not run | Not run | Pending | Run the host config below and record the observed version. |

## Next Host Tasks

- Open this workspace in VS Code and verify that `.vscode/mcp.json` exposes the
  `wuwe` and `wuwe-rag` servers in the MCP UI.
- In VS Code, call `echo_text`, call `preview_image`, read
  `wuwe://example/readme`, fetch `echo_prompt`, and call RAG
  `search_knowledge`.
- Record the exact VS Code version, date, pass/fail status, and any UI-specific
  limitation in the matrix above.
- Install or locate Cursor, Claude Desktop, and Continue, then repeat the same
  visibility and call tests with their preferred MCP configuration shape.
- For every real host, note whether sampling and elicitation are fulfilled,
  ignored, or reported as unsupported.

Use the absolute executable path in host configuration:

```powershell
Resolve-Path .\build\examples\Debug\mcp_stdio_example.exe
```

## Host Config Template

VS Code workspace configuration is checked in at `.vscode/mcp.json`:

```json
{
  "servers": {
    "wuwe": {
      "type": "stdio",
      "command": "${workspaceFolder}\\build\\examples\\Debug\\mcp_stdio_example.exe",
      "args": []
    },
    "wuwe-rag": {
      "type": "stdio",
      "command": "${workspaceFolder}\\build\\examples\\Debug\\knowledge_mcp_example.exe",
      "args": []
    }
  }
}
```

Most desktop MCP hosts use a JSON object that maps a server name to a command.
Use the absolute path returned by `Resolve-Path`:

```json
{
  "mcpServers": {
    "wuwe": {
      "command": "D:\\Miles Li\\Wuwe\\build\\examples\\Debug\\mcp_stdio_example.exe",
      "args": []
    }
  }
}
```

Some hosts use per-project or per-workspace configuration. Keep the same
`command` and `args` shape when the host supports it.

## Expected Handshake

A compatible host should:

1. Send `initialize`.
2. Receive capabilities for `tools`, `resources`, `roots`, and `prompts`.
3. Send `notifications/initialized`.
4. Optionally send `ping`.
5. Call `tools/list`, `resources/list`, and `prompts/list`.

The server should expose:

- `echo_text`
- `preview_image`
- `wuwe://example/readme`
- `wuwe://example/{name}`
- `echo_prompt`

For RAG-specific smoke testing, use `knowledge_mcp_example.exe` instead of
`mcp_stdio_example.exe`; the expected tool is `search_knowledge`.

## Manual Smoke Messages

If the host has a raw MCP inspector, use these JSON-RPC messages. The host tool
should handle Content-Length framing for you.

Initialize:

```json
{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","clientInfo":{"name":"compat-host","version":"0.1.0"},"capabilities":{}}}
```

Initialized notification:

```json
{"jsonrpc":"2.0","method":"notifications/initialized"}
```

Tools:

```json
{"jsonrpc":"2.0","id":2,"method":"tools/list"}
```

```json
{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"echo_text","arguments":{"text":"hello from host"}}}
```

Resources:

```json
{"jsonrpc":"2.0","id":4,"method":"resources/list"}
```

```json
{"jsonrpc":"2.0","id":5,"method":"resources/read","params":{"uri":"wuwe://example/readme"}}
```

Prompts:

```json
{"jsonrpc":"2.0","id":6,"method":"prompts/list"}
```

```json
{"jsonrpc":"2.0","id":7,"method":"prompts/get","params":{"name":"echo_prompt","arguments":{"topic":"host compatibility"}}}
```

Roots, when the embedding application configured them:

```json
{"jsonrpc":"2.0","id":9,"method":"roots/list"}
```

Rich content:

```json
{"jsonrpc":"2.0","id":8,"method":"tools/call","params":{"name":"preview_image","arguments":{}}}
```

## Pass Criteria

- The host starts `mcp_stdio_example.exe` without stderr protocol noise.
- The host sees both tools.
- `echo_text` returns the submitted text.
- `preview_image` returns an `image/png` content item.
- The resource can be listed and read.
- The prompt can be listed and fetched.
- Configured roots can be listed without exposing paths the host did not allow.
- Server-initiated sampling or elicitation requests are either fulfilled by the
  host or clearly reported as unsupported by that host.
- `ping` returns an empty result object.

## Troubleshooting

- Use an absolute executable path. Relative paths are interpreted differently by
  different hosts.
- Do not pipe line-delimited JSON into `mcp_stdio_example`; the real example
  expects Content-Length framed stdio. `run_lines()` exists only for tests and
  custom debug harnesses.
- Make sure the example does not print normal log lines to stdout. Stdout is the
  MCP protocol stream.
- If a host cannot display image content, verify that the raw response contains
  `{"type":"image","mimeType":"image/png","data":"..."}`.
- If a host only supports tools, `resources/*` and `prompts/*` may be ignored
  even though the server implements them.
