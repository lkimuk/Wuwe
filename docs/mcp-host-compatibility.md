---
id: mcp-host-compatibility
title: MCP host compatibility
description: Configure and verify Wuwe stdio servers in MCP hosts.
---

# MCP host compatibility

Wuwe's stdio server supports the two framing styles exercised by repository tests:

- Content-Length framed JSON-RPC;
- one JSON-RPC object per line.

The server replies using the framing style of the incoming request. Protocol version `2024-11-05` is the current Wuwe compatibility target.

## Host configuration

Build the example and use its absolute path in the host configuration:

```powershell
cmake --build build-vcpkg --config Release --target mcp_stdio_example
Resolve-Path .\build-vcpkg\examples\Release\mcp_stdio_example.exe
```

A common desktop-host shape is:

```json
{
  "mcpServers": {
    "wuwe": {
      "command": "D:\\path\\to\\mcp_stdio_example.exe",
      "args": []
    }
  }
}
```

Configuration file names and nesting differ by host. Preserve the executable, arguments, working directory, and required environment variables when adapting the entry.

## Expected protocol surface

The example exposes:

- tools `echo_text` and `preview_image`;
- resource `wuwe://example/readme`;
- resource template `wuwe://example/{name}`;
- prompt `echo_prompt`.

A normal host sequence is:

1. `initialize`;
2. `notifications/initialized`;
3. optional `ping`;
4. `tools/list`, `resources/list`, `resources/templates/list`, and `prompts/list`;
5. tool calls or resource and prompt reads.

The library also implements roots, subscriptions, list-change notifications, progress, logging, sampling, and elicitation. A host may expose only a subset of these features in its UI.

## Verification

Run the MCP unit suite and the process transcript:

```powershell
ctest --test-dir build-vcpkg -C Release -R mcp --output-on-failure
.\tools\mcp-host-transcript.ps1 -ServerPath build-vcpkg\examples\Release\mcp_stdio_example.exe
```

Pass criteria:

- the host starts the process without protocol text on stdout;
- initialization succeeds;
- listed tools, resources, templates, and prompts match the example;
- `echo_text` returns its input;
- the image result contains an `image/png` content item;
- the resource can be read and the prompt can be fetched;
- `ping` returns an empty result object.

For RAG verification, run `knowledge_mcp_example` and call `search_knowledge`.

## Troubleshooting

- Prefer an absolute executable path.
- Keep logs on stderr; stdout is the MCP transport.
- Pass API keys and other environment values through the host's server configuration.
- Capture raw stdin, stdout, and stderr before changing framing logic.
- If rich content is invisible in the UI, inspect the raw MCP response; host rendering support varies.
- Treat sampling and elicitation as negotiated capabilities. Do not assume every host will service server-initiated requests.
