import { spawnSync } from "node:child_process";
import { existsSync } from "node:fs";
import path from "node:path";

function parseArgs(argv) {
  const options = {
    server: path.join("build", "examples", "Debug", "mcp_stdio_example.exe"),
    build: false,
  };

  for (let i = 0; i < argv.length; ++i) {
    const arg = argv[i];
    if (arg === "--build") {
      options.build = true;
    } else if (arg === "--server") {
      options.server = argv[++i];
    } else {
      throw new Error(`unknown argument: ${arg}`);
    }
  }

  return options;
}

function run(command, args) {
  const result = spawnSync(command, args, {
    cwd: process.cwd(),
    shell: process.platform === "win32",
    stdio: "inherit",
  });
  if (result.status !== 0) {
    throw new Error(`${command} failed with exit code ${result.status}`);
  }
}

function assert(condition, message) {
  if (!condition) {
    throw new Error(message);
  }
}

function frame(message) {
  const body = Buffer.from(JSON.stringify(message), "utf8");
  return Buffer.concat([
    Buffer.from(`Content-Length: ${body.length}\r\n\r\n`, "ascii"),
    body,
  ]);
}

function parseFrames(buffer) {
  const messages = [];
  let remaining = buffer;
  while (remaining.length > 0) {
    const headerEnd = remaining.indexOf("\r\n\r\n");
    if (headerEnd < 0) {
      throw new Error(
        `trailing stdout did not contain a complete MCP header: ${JSON.stringify(remaining.toString("utf8"))}`);
    }

    const header = remaining.subarray(0, headerEnd).toString("ascii");
    const match = /content-length:\s*(\d+)/i.exec(header);
    if (!match) {
      throw new Error(`MCP frame missing Content-Length: ${header}`);
    }

    const length = Number.parseInt(match[1], 10);
    const frameEnd = headerEnd + 4 + length;
    if (remaining.length < frameEnd) {
      throw new Error("trailing stdout did not contain a complete MCP body");
    }

    const body = remaining.subarray(headerEnd + 4, frameEnd).toString("utf8");
    messages.push(JSON.parse(body));
    remaining = remaining.subarray(frameEnd);
  }
  return messages;
}

function responseById(messages, id) {
  const response = messages.find((message) => message.id === id);
  assert(response !== undefined, `missing response id ${id}`);
  assert(response.error === undefined, `response id ${id} failed: ${JSON.stringify(response.error)}`);
  return response;
}

function main() {
  const options = parseArgs(process.argv.slice(2));
  if (options.build) {
    run("cmake", [
      "--build",
      "build",
      "--config",
      "Debug",
      "--target",
      "mcp_stdio_example",
    ]);
  }

  const serverPath = path.resolve(options.server);
  assert(existsSync(serverPath), `server executable does not exist: ${serverPath}`);

  const input = Buffer.concat([
    frame({
      jsonrpc: "2.0",
      id: 1,
      method: "initialize",
      params: {
      protocolVersion: "2024-11-05",
      clientInfo: { name: "wuwe-host-transcript", version: "0.1.0" },
      capabilities: {
        roots: { listChanged: true },
        sampling: {},
        elicitation: {},
      },
      },
    }),
    frame({ jsonrpc: "2.0", method: "notifications/initialized" }),
    frame({ jsonrpc: "2.0", id: 2, method: "ping" }),
    frame({ jsonrpc: "2.0", id: 3, method: "tools/list" }),
    frame({
      jsonrpc: "2.0",
      id: 4,
      method: "tools/call",
      params: { name: "echo_text", arguments: { text: "hello from host" } },
    }),
    frame({
      jsonrpc: "2.0",
      id: 5,
      method: "tools/call",
      params: { name: "preview_image", arguments: {} },
    }),
    frame({ jsonrpc: "2.0", id: 6, method: "resources/list" }),
    frame({
      jsonrpc: "2.0",
      id: 7,
      method: "resources/read",
      params: { uri: "wuwe://example/readme" },
    }),
    frame({ jsonrpc: "2.0", id: 8, method: "prompts/list" }),
    frame({
      jsonrpc: "2.0",
      id: 9,
      method: "prompts/get",
      params: { name: "echo_prompt", arguments: { topic: "host compatibility" } },
    }),
  ]);

  const result = spawnSync(serverPath, [], {
    cwd: process.cwd(),
    input,
    maxBuffer: 1024 * 1024,
    windowsHide: true,
  });
  if (result.status !== 0) {
    throw new Error(`MCP server exited with code ${result.status}: ${result.stderr.toString("utf8")}`);
  }
  const messages = parseFrames(result.stdout);

  console.log("Validating initialize");
  const initialize = responseById(messages, 1);
  assert(initialize.result.capabilities.tools.listChanged === true,
      "initialize should expose tools/listChanged");
  assert(initialize.result.capabilities.resources.subscribe === true,
      "initialize should expose resource subscriptions");
  assert(initialize.result.capabilities.prompts.listChanged === true,
      "initialize should expose prompts/listChanged");

  console.log("Validating ping");
  const ping = responseById(messages, 2);
  assert(typeof ping.result === "object" && !Array.isArray(ping.result),
      "ping should return an empty result object");

  console.log("Validating tools");
  const tools = responseById(messages, 3);
  assert(tools.result.tools.some((tool) => tool.name === "echo_text"),
      "tools/list should expose echo_text");
  assert(tools.result.tools.some((tool) => tool.name === "preview_image"),
      "tools/list should expose preview_image");

  const echo = responseById(messages, 4);
  assert(echo.result.content[0].text === "hello from host",
      "echo_text should return the submitted text");

  const image = responseById(messages, 5);
  assert(image.result.content[0].type === "image", "preview_image should return image content");
  assert(image.result.content[0].mimeType === "image/png", "preview_image should return image/png");

  console.log("Validating resources");
  const resources = responseById(messages, 6);
  assert(resources.result.resources.some((resource) => resource.uri === "wuwe://example/readme"),
      "resources/list should expose example README");

  const resource = responseById(messages, 7);
  assert(resource.result.contents[0].text.includes("Wuwe MCP stdio example"),
      "resources/read should return README text");

  console.log("Validating prompts");
  const prompts = responseById(messages, 8);
  assert(prompts.result.prompts.some((prompt) => prompt.name === "echo_prompt"),
      "prompts/list should expose echo_prompt");

  const prompt = responseById(messages, 9);
  assert(prompt.result.messages[0].content.text.includes("host compatibility"),
      "prompts/get should return the requested topic");

  const stderr = result.stderr.toString("utf8").trim();
  if (stderr.length > 0) {
    console.warn(stderr);
  }
  console.log(`MCP host transcript passed for ${serverPath}`);
}

try {
  main();
} catch (error) {
  console.error(error instanceof Error ? error.message : String(error));
  process.exitCode = 1;
}
