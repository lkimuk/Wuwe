#include <chrono>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>
#include <httplib/httplib.h>

#include <wuwe/agent/mcp/mcp_async.hpp>
#include <wuwe/agent/mcp/mcp_gateway.hpp>
#include <wuwe/agent/mcp/mcp_host_runtime.hpp>
#include <wuwe/agent/mcp/mcp_host_telemetry.hpp>
#include <wuwe/agent/mcp/mcp_http_listener.hpp>
#include <wuwe/agent/mcp/mcp_process_client.hpp>
#include <wuwe/agent/mcp/mcp_server.hpp>
#include <wuwe/agent/mcp/mcp_http_transport.hpp>
#include <wuwe/agent/mcp/mcp_stdio_client.hpp>
#include <wuwe/agent/mcp/mcp_stdio_transport.hpp>
#include <wuwe/agent/tools/tool.hpp>
#include <wuwe/common/print.h>

using json = nlohmann::json;

void require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

json parse_response(const std::optional<std::string>& response) {
  require(response.has_value(), "expected JSON-RPC response");
  return json::parse(*response);
}

struct echo_text {
  static constexpr std::string_view description = "Echo text for MCP tests.";

  std::string text;

  std::string invoke() const {
    return text;
  }
};

namespace {

void test_initialize_reports_server_capabilities() {
  wuwe::agent::mcp::mcp_server server({ .name = "wuwe-test", .version = "9.9.9" });

  const auto response = parse_response(server.handle_message(R"({
    "jsonrpc":"2.0",
    "id":1,
    "method":"initialize",
    "params":{"protocolVersion":"2024-11-05"}
  })"));

  require(response["id"] == 1, "initialize should preserve request id");
  require(response["result"]["serverInfo"]["name"] == "wuwe-test",
    "initialize should report configured server name");
  require(response["result"]["capabilities"].contains("tools"),
    "initialize should advertise tools capability");
  require(response["result"]["capabilities"].contains("resources"),
    "initialize should advertise resources capability");
  require(response["result"]["capabilities"].contains("prompts"),
    "initialize should advertise prompts capability");
  require(response["result"]["capabilities"].contains("roots"),
    "initialize should advertise roots capability");
  require(response["result"]["capabilities"]["resources"]["subscribe"].get<bool>(),
    "initialize should advertise resource subscription support");
  require(server.client_info().name.empty(),
    "initialize without clientInfo should leave client name empty");
}

void test_initialize_records_client_info_and_capabilities() {
  wuwe::agent::mcp::mcp_server server;

  const auto response = parse_response(server.handle_message(R"({
    "jsonrpc":"2.0",
    "id":28,
    "method":"initialize",
    "params":{
      "protocolVersion":"2024-11-05",
      "clientInfo":{"name":"test-host","version":"1.2.3"},
      "capabilities":{"roots":{"listChanged":true}}
    }
  })"));

  require(response["result"]["protocolVersion"] == wuwe::agent::mcp::default_protocol_version,
    "initialize should return server protocol version");
  require(server.client_info().name == "test-host",
    "initialize should record client name");
  require(server.client_info().version == "1.2.3",
    "initialize should record client version");
  require(server.client_capabilities()["roots"]["listChanged"].get<bool>(),
    "initialize should record client capabilities");
}

void test_initialized_notification_sets_state_and_ping_responds() {
  wuwe::agent::mcp::mcp_server server;

  require(!server.initialized(), "server should start before initialized notification");
  const auto notification = server.handle_message(R"({
    "jsonrpc":"2.0",
    "method":"notifications/initialized"
  })");
  require(!notification.has_value(), "initialized notification should not return response");
  require(server.initialized(), "initialized notification should set initialized state");

  const auto ping = parse_response(server.handle_message(R"({
    "jsonrpc":"2.0",
    "id":29,
    "method":"ping"
  })"));
  require(ping["result"].is_object(), "ping should return an empty result object");
}

void test_tools_list_exposes_provider_schema() {
  wuwe::tool_provider<echo_text> provider;
  wuwe::agent::mcp::mcp_server server;
  server.add_tool_provider(provider);

  const auto response = parse_response(server.handle_message(R"({
    "jsonrpc":"2.0",
    "id":"list-1",
    "method":"tools/list"
  })"));

  const auto tools = response["result"]["tools"];
  require(tools.size() == 1, "tools/list should expose registered provider tools");
  require(tools[0]["name"] == "echo_text", "tools/list should use llm tool names");
  require(tools[0]["inputSchema"]["properties"].contains("text"),
    "tools/list should expose reflected input schema");
}

void test_list_methods_support_cursor_pagination() {
  wuwe::agent::mcp::mcp_server server;
  server.set_list_page_size(1);
  server.add_mcp_tool(
    { .name = "first_tool", .description = "First." },
    [](const json&) {
      return wuwe::agent::mcp::mcp_tool_call_result {};
    });
  server.add_mcp_tool(
    { .name = "second_tool", .description = "Second." },
    [](const json&) {
      return wuwe::agent::mcp::mcp_tool_call_result {};
    });
  server.add_resource(
    { .uri = "wuwe://first", .name = "First" },
    [] {
      return std::vector<wuwe::agent::mcp::mcp_resource_content> {};
    });
  server.add_resource(
    { .uri = "wuwe://second", .name = "Second" },
    [] {
      return std::vector<wuwe::agent::mcp::mcp_resource_content> {};
    });
  server.add_prompt({ .name = "first_prompt" }, [](const json&) {
    return wuwe::agent::mcp::mcp_prompt_result {};
  });
  server.add_prompt({ .name = "second_prompt" }, [](const json&) {
    return wuwe::agent::mcp::mcp_prompt_result {};
  });

  const auto first_tools = parse_response(server.handle_message(R"({
    "jsonrpc":"2.0",
    "id":24,
    "method":"tools/list"
  })"));
  require(first_tools["result"]["tools"].size() == 1,
    "tools/list should page results when page size is set");
  require(first_tools["result"].contains("nextCursor"),
    "tools/list should return nextCursor when more results exist");

  const auto second_tools = parse_response(server.handle_message(R"({
    "jsonrpc":"2.0",
    "id":25,
    "method":"tools/list",
    "params":{"cursor":"1"}
  })"));
  require(second_tools["result"]["tools"].size() == 1,
    "tools/list should read the next cursor page");
  require(!second_tools["result"].contains("nextCursor"),
    "last tools/list page should omit nextCursor");

  const auto resources = parse_response(server.handle_message(R"({
    "jsonrpc":"2.0",
    "id":26,
    "method":"resources/list"
  })"));
  require(resources["result"]["resources"].size() == 1,
    "resources/list should page results when page size is set");
  require(resources["result"].contains("nextCursor"),
    "resources/list should return nextCursor when more results exist");

  const auto prompts = parse_response(server.handle_message(R"({
    "jsonrpc":"2.0",
    "id":27,
    "method":"prompts/list"
  })"));
  require(prompts["result"]["prompts"].size() == 1,
    "prompts/list should page results when page size is set");
  require(prompts["result"].contains("nextCursor"),
    "prompts/list should return nextCursor when more results exist");
}

void test_tools_call_invokes_provider() {
  wuwe::tool_provider<echo_text> provider;
  wuwe::agent::mcp::mcp_server server;
  server.add_tool_provider(provider);

  const auto response = parse_response(server.handle_message(R"({
    "jsonrpc":"2.0",
    "id":2,
    "method":"tools/call",
    "params":{"name":"echo_text","arguments":{"text":"hello mcp"}}
  })"));

  require(!response["result"]["isError"].get<bool>(), "successful tool call should not be an error");
  require(response["result"]["content"][0]["type"] == "text",
    "tool call result should use MCP text content");
  require(response["result"]["content"][0]["text"] == "hello mcp",
    "tool call should return provider content");

  const auto record = server.request_registry().get("2");
  require(record.has_value(), "tool calls should be recorded in request registry");
  require(record->method == "tools/call", "request registry should record method");
  require(record->target == "echo_text", "request registry should record target");
  require(record->state == wuwe::agent::mcp::mcp_request_state::completed,
    "successful tool calls should complete lifecycle record");
}

void test_mcp_tool_can_return_image_content() {
  wuwe::agent::mcp::mcp_server server;
  server.add_mcp_tool(
    {
      .name = "preview_image",
      .description = "Return a tiny image payload.",
      .parameters_json_schema = R"({"type":"object","properties":{},"additionalProperties":false})",
    },
    [](const json&) {
      return wuwe::agent::mcp::mcp_tool_call_result {
        .content = {
          wuwe::agent::mcp::mcp_content::image("iVBORw0KGgo="),
        },
      };
    });

  const auto response = parse_response(server.handle_message(R"({
    "jsonrpc":"2.0",
    "id":19,
    "method":"tools/call",
    "params":{"name":"preview_image","arguments":{}}
  })"));

  const auto content = response["result"]["content"][0];
  require(content["type"] == "image", "direct MCP tools should support image content");
  require(content["data"] == "iVBORw0KGgo=", "image content should include base64 data");
  require(content["mimeType"] == "image/png", "image content should include mime type");
}

void test_invalid_arguments_become_tool_error_result() {
  wuwe::tool_provider<echo_text> provider;
  wuwe::agent::mcp::mcp_server server;
  server.add_tool_provider(provider);

  const auto response = parse_response(server.handle_message(R"({
    "jsonrpc":"2.0",
    "id":3,
    "method":"tools/call",
    "params":{"name":"echo_text","arguments":{}}
  })"));

  require(response["result"]["isError"].get<bool>(),
    "provider argument errors should become MCP tool error results");
  require(response["result"]["content"][0]["text"].get<std::string>().find("invalid arguments") !=
      std::string::npos,
    "tool error result should include provider error text");
}

void test_invalid_json_and_unknown_tool_return_jsonrpc_errors() {
  wuwe::agent::mcp::mcp_server server;

  const auto parse_error = parse_response(server.handle_message("{"));
  require(parse_error["error"]["code"] == -32700, "invalid JSON should return parse error");

  const auto unknown_tool = parse_response(server.handle_message(R"({
    "jsonrpc":"2.0",
    "id":4,
    "method":"tools/call",
    "params":{"name":"missing","arguments":{}}
  })"));
  require(unknown_tool["error"]["code"] == -32602,
    "unknown tool should return invalid params");
}

void test_jsonrpc_batch_returns_response_array_and_skips_notifications() {
  wuwe::tool_provider<echo_text> provider;
  wuwe::agent::mcp::mcp_server server;
  server.add_tool_provider(provider);

  const auto response = server.handle_message(R"([
    {"jsonrpc":"2.0","id":30,"method":"ping"},
    {"jsonrpc":"2.0","method":"notifications/initialized"},
    {"jsonrpc":"2.0","id":31,"method":"tools/call","params":{"name":"echo_text","arguments":{"text":"batch"}}}
  ])");

  require(response.has_value(), "batch requests with calls should return a response array");
  const auto batch = json::parse(*response);
  require(batch.is_array(), "batch response should be an array");
  require(batch.size() == 2, "batch response should omit notification entries");
  require(batch[0]["id"] == 30, "batch response should include first request id");
  require(batch[1]["id"] == 31, "batch response should include second request id");
  require(batch[1]["result"]["content"][0]["text"] == "batch",
    "batch response should include tool result");
  require(server.initialized(), "batch notifications should still be processed");
}

void test_empty_jsonrpc_batch_is_invalid() {
  wuwe::agent::mcp::mcp_server server;

  const auto response = parse_response(server.handle_message("[]"));

  require(response["error"]["code"] == -32600, "empty batch should return invalid request");
}

void test_batch_reports_malformed_items_and_params() {
  wuwe::tool_provider<echo_text> provider;
  wuwe::agent::mcp::mcp_server server;
  server.add_tool_provider(provider);

  const auto response = parse_response(server.handle_message(R"([
    7,
    {"jsonrpc":"2.0","id":51,"method":"tools/call","params":false},
    {"jsonrpc":"2.0","id":52,"method":"tools/call","params":{"name":"echo_text","arguments":[]}},
    {"jsonrpc":"2.0","id":53,"method":"tools/call","params":{"name":"echo_text","arguments":{"text":"ok"}}}
  ])"));

  require(response.is_array(), "malformed batch should still return an array response");
  require(response.size() == 4, "batch should return one response per request-like item");
  require(response[0]["error"]["code"] == -32600,
    "primitive batch items should return invalid request");
  require(response[1]["error"]["code"] == -32602,
    "non-object params should return invalid params");
  require(response[2]["error"]["code"] == -32602,
    "malformed arguments should return invalid params");
  require(response[3]["result"]["content"][0]["text"] == "ok",
    "valid batch items should still execute after malformed items");
}

void test_notifications_do_not_write_responses() {
  wuwe::agent::mcp::mcp_server server;

  const auto response = server.handle_message(R"({
    "jsonrpc":"2.0",
    "method":"notifications/initialized"
  })");

  require(!response.has_value(), "JSON-RPC notifications should not return responses");
}

void test_cancelled_notification_tracks_request_id() {
  wuwe::agent::mcp::mcp_server server;

  const auto response = server.handle_message(R"({
    "jsonrpc":"2.0",
    "method":"notifications/cancelled",
    "params":{"requestId":"job-1","reason":"user cancelled"}
  })");

  require(!response.has_value(), "cancelled notifications should not return responses");
  require(server.is_cancelled("job-1"), "server should track cancelled request ids");
}

void test_tools_can_emit_log_and_progress_notifications() {
  wuwe::agent::mcp::mcp_server server;
  server.add_mcp_tool(
    {
      .name = "long_task",
      .description = "Emit progress and log notifications.",
      .parameters_json_schema = R"({"type":"object","properties":{},"additionalProperties":false})",
    },
    [&server](const json&) {
      server.emit_log("info", "long task started", "mcp_tests");
      server.emit_progress("task-1", 50.0, 100.0, "halfway");
      return wuwe::agent::mcp::mcp_tool_call_result {
        .content = { { .type = "text", .text = "done" } },
      };
    });

  const auto exchange = server.handle_message_exchange(R"({
    "jsonrpc":"2.0",
    "id":22,
    "method":"tools/call",
    "params":{"name":"long_task","arguments":{}}
  })");

  require(exchange.notifications.size() == 2,
    "tool callbacks should be able to emit notifications before the response");
  const auto log = json::parse(exchange.notifications[0]);
  const auto progress = json::parse(exchange.notifications[1]);
  require(log["method"] == "notifications/message", "first notification should be a log message");
  require(log["params"]["data"]["message"] == "long task started",
    "log notification should include message");
  require(progress["method"] == "notifications/progress",
    "second notification should be progress");
  require(progress["params"]["progress"] == 50.0, "progress notification should include progress");
  require(exchange.response.has_value(), "exchange should include final response");
}

void test_request_progress_updates_lifecycle_record() {
  wuwe::agent::mcp::mcp_server server;
  server.add_mcp_tool(
    {
      .name = "progress_task",
      .description = "Update lifecycle progress.",
      .parameters_json_schema = R"({"type":"object","properties":{},"additionalProperties":false})",
    },
    [&server](const json&) {
      server.emit_request_progress(37, "progress-37", 25.0, 100.0, "quarter");
      return wuwe::agent::mcp::mcp_tool_call_result::text("ok");
    });

  const auto exchange = server.handle_message_exchange(R"({
    "jsonrpc":"2.0",
    "id":37,
    "method":"tools/call",
    "params":{"name":"progress_task","arguments":{}}
  })");

  require(exchange.notifications.size() == 1,
    "request progress should emit one progress notification");
  const auto record = server.request_registry().get("37");
  require(record.has_value(), "request progress should have lifecycle record");
  require(record->progress == 25.0, "request progress should update registry progress");
  require(record->total && *record->total == 100.0,
    "request progress should update registry total");
  require(record->progress_message == "quarter",
    "request progress should update registry message");
}

void test_request_timeout_marks_lifecycle_failed() {
  wuwe::agent::mcp::mcp_server server;
  server.set_request_timeout(std::chrono::milliseconds(1));
  server.add_mcp_tool(
    { .name = "slow_tool", .description = "Sleeps." },
    [](const json&) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      return wuwe::agent::mcp::mcp_tool_call_result::text("late");
    });

  const auto response = parse_response(server.handle_message(R"({
    "jsonrpc":"2.0",
    "id":38,
    "method":"tools/call",
    "params":{"name":"slow_tool","arguments":{}}
  })"));

  require(response["error"]["code"] == -32603,
    "timed out requests should return internal error");
  const auto record = server.request_registry().get("38");
  require(record.has_value(), "timed out request should have lifecycle record");
  require(record->state == wuwe::agent::mcp::mcp_request_state::failed,
    "timed out request should mark lifecycle failed");
  require(record->error == "request timed out",
    "timed out request should record timeout error");
}

void test_sampling_and_elicitation_requests_round_trip_through_exchange() {
  wuwe::agent::mcp::mcp_server server;
  server.add_mcp_tool(
    {
      .name = "ask_host",
      .description = "Ask host for sampling and user input.",
      .parameters_json_schema = R"({"type":"object","properties":{}})",
    },
    [&server](const json&) {
      const auto sampling_id = server.request_sampling(json {
        { "messages", json::array({
          {
            { "role", "user" },
            { "content", { { "type", "text" }, { "text", "Draft a title." } } },
          },
        }) },
        { "maxTokens", 32 },
      });
      const auto elicitation_id = server.request_elicitation(json {
        { "message", "Choose a project name." },
        { "requestedSchema", {
          { "type", "object" },
          { "properties", {
            { "name", { { "type", "string" } } },
          } },
        } },
      });
      return wuwe::agent::mcp::mcp_tool_call_result::text(
        sampling_id.dump() + ":" + elicitation_id.dump());
    });

  const auto exchange = server.handle_message_exchange(R"({
    "jsonrpc":"2.0",
    "id":39,
    "method":"tools/call",
    "params":{"name":"ask_host","arguments":{}}
  })");

  require(exchange.requests.size() == 2,
    "sampling and elicitation should emit outbound client requests");
  const auto sampling_request = json::parse(exchange.requests[0]);
  const auto elicitation_request = json::parse(exchange.requests[1]);
  require(sampling_request["method"] == "sampling/createMessage",
    "sampling helper should use sampling/createMessage");
  require(sampling_request["params"]["maxTokens"] == 32,
    "sampling helper should preserve params");
  require(elicitation_request["method"] == "elicitation/create",
    "elicitation helper should use elicitation/create");
  require(elicitation_request["params"]["requestedSchema"]["type"] == "object",
    "elicitation helper should preserve requested schema");

  require(!server.client_request(sampling_request["id"])->completed,
    "outbound request should start pending");
  const auto sampling_response = server.handle_message(json {
    { "jsonrpc", "2.0" },
    { "id", sampling_request["id"] },
    { "result", {
      { "role", "assistant" },
      { "content", { { "type", "text" }, { "text", "A title" } } },
    } },
  }.dump());
  require(!sampling_response.has_value(),
    "JSON-RPC responses to outbound requests should not produce responses");
  const auto completed_sampling = server.client_request(sampling_request["id"]);
  require(completed_sampling && completed_sampling->completed,
    "outbound request response should complete pending record");
  require(completed_sampling->result["content"]["text"] == "A title",
    "outbound request should store result payload");
  server.clear_completed_client_requests();
  require(!server.client_request(sampling_request["id"]).has_value(),
    "server should clear completed outbound request records");

  const auto elicitation_response = server.handle_message(json {
    { "jsonrpc", "2.0" },
    { "id", elicitation_request["id"] },
    { "error", {
      { "code", -32000 },
      { "message", "user declined" },
    } },
  }.dump());
  require(!elicitation_response.has_value(),
    "JSON-RPC errors to outbound requests should not produce responses");
  const auto failed_elicitation = server.client_request(elicitation_request["id"]);
  require(failed_elicitation && failed_elicitation->completed,
    "outbound error should complete pending record");
  require(failed_elicitation->error.has_value(),
    "outbound error should be stored");
  require(failed_elicitation->error->message == "user declined",
    "outbound error should store message");
}

void test_sampling_and_elicitation_follow_client_capabilities() {
  wuwe::agent::mcp::mcp_server server;

  parse_response(server.handle_message(R"({
    "jsonrpc":"2.0",
    "id":61,
    "method":"initialize",
    "params":{"protocolVersion":"2024-11-05","capabilities":{}}
  })"));
  const auto denied_sampling = server.request_sampling(json { { "messages", json::array() } });
  const auto denied_elicitation = server.request_elicitation(json { { "message", "Name?" } });
  require(denied_sampling.is_null(),
    "sampling request should not be enqueued without client sampling capability");
  require(denied_elicitation.is_null(),
    "elicitation request should not be enqueued without client elicitation capability");
  require(server.pending_client_requests().empty(),
    "unsupported outbound requests should not leave pending records");

  wuwe::agent::mcp::mcp_server capable_server;
  parse_response(capable_server.handle_message(R"({
    "jsonrpc":"2.0",
    "id":62,
    "method":"initialize",
    "params":{
      "protocolVersion":"2024-11-05",
      "capabilities":{"sampling":{},"elicitation":{}}
    }
  })"));
  const auto sampling_id = capable_server.request_sampling(json { { "messages", json::array() } });
  const auto elicitation_id = capable_server.request_elicitation(json { { "message", "Name?" } });
  require(sampling_id.is_number_integer(),
    "sampling request should be enqueued when client declares capability");
  require(elicitation_id.is_number_integer(),
    "elicitation request should be enqueued when client declares capability");
  require(capable_server.pending_client_requests().size() == 2,
    "capable client should leave pending outbound records");
}

void test_typed_sampling_and_elicitation_helpers() {
  wuwe::agent::mcp::mcp_server server;
  const auto sampling_id = server.request_sampling(wuwe::agent::mcp::mcp_sampling_request {
    .messages = {
      wuwe::agent::mcp::mcp_sampling_message::user_text("Draft a title."),
    },
    .max_tokens = 64,
    .temperature = 0.2,
    .metadata = { { "trace", "typed" } },
  });
  const auto elicitation_id = server.request_elicitation(wuwe::agent::mcp::mcp_elicitation_request {
    .message = "Choose a name.",
    .requested_schema = {
      { "type", "object" },
      { "properties", {
        { "name", { { "type", "string" } } },
      } },
    },
  });

  const auto sampling = server.client_request(sampling_id);
  const auto elicitation = server.client_request(elicitation_id);
  require(sampling && sampling->params["messages"][0]["content"]["text"] == "Draft a title.",
    "typed sampling helper should serialize text content");
  require(sampling->params["maxTokens"] == 64,
    "typed sampling helper should serialize max tokens");
  require(sampling->params["temperature"] == 0.2,
    "typed sampling helper should serialize temperature");
  require(elicitation && elicitation->params["requestedSchema"]["properties"].contains("name"),
    "typed elicitation helper should serialize requested schema");

  const auto sampling_result = wuwe::agent::mcp::mcp_sampling_result::from_json({
    { "role", "assistant" },
    { "content", { { "type", "text" }, { "text", "A title" } } },
    { "model", "test-model" },
    { "stopReason", "endTurn" },
  });
  require(sampling_result && sampling_result->content.text == "A title",
    "typed sampling result should parse content");
  require(sampling_result->model == "test-model",
    "typed sampling result should parse model");

  const auto elicitation_result = wuwe::agent::mcp::mcp_elicitation_result::from_json({
    { "action", "accept" },
    { "content", { { "name", "Wuwe" } } },
  });
  require(elicitation_result && elicitation_result->content["name"] == "Wuwe",
    "typed elicitation result should parse content");
}

void test_resources_list_and_read_registered_content() {
  wuwe::agent::mcp::mcp_server server;
  server.add_resource(
    {
      .uri = "wuwe://docs/intro",
      .name = "Intro",
      .description = "A tiny test resource.",
      .mime_type = "text/markdown",
    },
    [] {
      return std::vector<wuwe::agent::mcp::mcp_resource_content> {
        {
          .uri = "wuwe://docs/intro",
          .mime_type = "text/markdown",
          .text = "# Intro\nHello resources.",
        },
      };
    });

  const auto list_response = parse_response(server.handle_message(R"({
    "jsonrpc":"2.0",
    "id":5,
    "method":"resources/list"
  })"));

  const auto resources = list_response["result"]["resources"];
  require(resources.size() == 1, "resources/list should expose registered resources");
  require(resources[0]["uri"] == "wuwe://docs/intro", "resources/list should include uri");
  require(resources[0]["mimeType"] == "text/markdown", "resources/list should include mime type");

  const auto read_response = parse_response(server.handle_message(R"({
    "jsonrpc":"2.0",
    "id":6,
    "method":"resources/read",
    "params":{"uri":"wuwe://docs/intro"}
  })"));

  const auto contents = read_response["result"]["contents"];
  require(contents.size() == 1, "resources/read should return resource contents");
  require(contents[0]["text"].get<std::string>().find("Hello resources") != std::string::npos,
    "resources/read should return text content");

  const auto record = server.request_registry().get("6");
  require(record.has_value(), "resource reads should be recorded in request registry");
  require(record->state == wuwe::agent::mcp::mcp_request_state::completed,
    "successful resource reads should complete lifecycle record");
}

void test_resources_read_can_return_blob_content() {
  wuwe::agent::mcp::mcp_server server;
  server.add_resource(
    {
      .uri = "wuwe://assets/tiny",
      .name = "Tiny Asset",
      .mime_type = "application/octet-stream",
    },
    [] {
      return std::vector<wuwe::agent::mcp::mcp_resource_content> {
        wuwe::agent::mcp::mcp_resource_content::blob_content(
          "wuwe://assets/tiny", "AAECAw==", "application/octet-stream"),
      };
    });

  const auto response = parse_response(server.handle_message(R"({
    "jsonrpc":"2.0",
    "id":20,
    "method":"resources/read",
    "params":{"uri":"wuwe://assets/tiny"}
  })"));

  const auto content = response["result"]["contents"][0];
  require(content["blob"] == "AAECAw==", "resources/read should support blob content");
  require(content["mimeType"] == "application/octet-stream",
    "blob resources should include mime type");
}

void test_resources_subscribe_unsubscribe_and_update_notifications() {
  wuwe::agent::mcp::mcp_server server;
  server.add_resource(
    { .uri = "wuwe://updates", .name = "Updates" },
    [] {
      return std::vector<wuwe::agent::mcp::mcp_resource_content> {
        { .uri = "wuwe://updates", .text = "updated" },
      };
    });
  server.add_mcp_tool(
    { .name = "touch_resource", .description = "Emit a resource update." },
    [&server](const json&) {
      server.emit_resource_updated("wuwe://updates");
      server.emit_resource_list_changed();
      server.emit_tool_list_changed();
      server.emit_prompt_list_changed();
      return wuwe::agent::mcp::mcp_tool_call_result {
        .content = { { .type = "text", .text = "touched" } },
      };
    });

  const auto subscribe = parse_response(server.handle_message(R"({
    "jsonrpc":"2.0",
    "id":32,
    "method":"resources/subscribe",
    "params":{"uri":"wuwe://updates"}
  })"));
  require(subscribe["result"].is_object(), "resources/subscribe should return empty result");
  require(server.is_resource_subscribed("wuwe://updates"),
    "resources/subscribe should mark resource subscribed");

  const auto exchange = server.handle_message_exchange(R"({
    "jsonrpc":"2.0",
    "id":33,
    "method":"tools/call",
    "params":{"name":"touch_resource","arguments":{}}
  })");
  require(exchange.notifications.size() == 4,
    "resource update and listChanged helpers should emit notifications");
  require(json::parse(exchange.notifications[0])["method"] == "notifications/resources/updated",
    "resource update notification should be emitted first");
  require(json::parse(exchange.notifications[1])["method"] ==
      "notifications/resources/list_changed",
    "resource listChanged notification should be emitted");
  require(json::parse(exchange.notifications[2])["method"] == "notifications/tools/list_changed",
    "tool listChanged notification should be emitted");
  require(json::parse(exchange.notifications[3])["method"] == "notifications/prompts/list_changed",
    "prompt listChanged notification should be emitted");

  const auto unsubscribe = parse_response(server.handle_message(R"({
    "jsonrpc":"2.0",
    "id":34,
    "method":"resources/unsubscribe",
    "params":{"uri":"wuwe://updates"}
  })"));
  require(unsubscribe["result"].is_object(), "resources/unsubscribe should return empty result");
  require(!server.is_resource_subscribed("wuwe://updates"),
    "resources/unsubscribe should clear subscription");
}

void test_resource_templates_list_registered_templates() {
  wuwe::agent::mcp::mcp_server server;
  server.add_resource_template({
    .uri_template = "wuwe://docs/{id}",
    .name = "Document",
    .description = "Document by id.",
    .mime_type = "text/plain",
  });

  const auto response = parse_response(server.handle_message(R"({
    "jsonrpc":"2.0",
    "id":7,
    "method":"resources/templates/list"
  })"));

  const auto templates = response["result"]["resourceTemplates"];
  require(templates.size() == 1, "resources/templates/list should expose templates");
  require(templates[0]["uriTemplate"] == "wuwe://docs/{id}",
    "resource template should include uriTemplate");
}

void test_prompts_list_and_get_registered_prompt() {
  wuwe::agent::mcp::mcp_server server;
  server.add_prompt(
    {
      .name = "summarize",
      .description = "Summarize input text.",
      .arguments = {
        {
          .name = "topic",
          .description = "Topic to emphasize.",
          .required = true,
        },
      },
    },
    [](const json& arguments) {
      const auto topic = arguments.value("topic", "general");
      return wuwe::agent::mcp::mcp_prompt_result {
        .description = "Summarize with a topic.",
        .messages = {
          {
            .role = "user",
            .text = "Summarize this material with focus on " + topic + ".",
          },
        },
      };
    });

  const auto list_response = parse_response(server.handle_message(R"({
    "jsonrpc":"2.0",
    "id":8,
    "method":"prompts/list"
  })"));

  const auto prompts = list_response["result"]["prompts"];
  require(prompts.size() == 1, "prompts/list should expose registered prompts");
  require(prompts[0]["name"] == "summarize", "prompts/list should include prompt name");
  require(prompts[0]["arguments"][0]["required"].get<bool>(),
    "prompts/list should include argument metadata");

  const auto get_response = parse_response(server.handle_message(R"({
    "jsonrpc":"2.0",
    "id":9,
    "method":"prompts/get",
    "params":{"name":"summarize","arguments":{"topic":"MCP"}}
  })"));

  const auto messages = get_response["result"]["messages"];
  require(messages.size() == 1, "prompts/get should return messages");
  require(messages[0]["content"]["text"].get<std::string>().find("MCP") != std::string::npos,
    "prompts/get should pass caller arguments to prompt callback");

  const auto record = server.request_registry().get("9");
  require(record.has_value(), "prompt gets should be recorded in request registry");
  require(record->state == wuwe::agent::mcp::mcp_request_state::completed,
    "successful prompt gets should complete lifecycle record");
}

void test_request_registry_records_failures() {
  wuwe::agent::mcp::mcp_server server;
  server.add_mcp_tool(
    { .name = "failing_tool", .description = "Throws." },
    [](const json&) -> wuwe::agent::mcp::mcp_tool_call_result {
      throw std::runtime_error("planned failure");
    });

  const auto response = parse_response(server.handle_message(R"({
    "jsonrpc":"2.0",
    "id":36,
    "method":"tools/call",
    "params":{"name":"failing_tool","arguments":{}}
  })"));

  require(response["error"]["code"] == -32603,
    "thrown tool callbacks should return internal error");
  const auto record = server.request_registry().get("36");
  require(record.has_value(), "failed tool calls should be recorded");
  require(record->state == wuwe::agent::mcp::mcp_request_state::failed,
    "failed tool calls should mark lifecycle record failed");
  require(record->error == "planned failure",
    "failed lifecycle record should include error message");
}

void test_prompts_get_can_return_non_text_content() {
  wuwe::agent::mcp::mcp_server server;
  server.add_prompt(
    { .name = "show_image" },
    [](const json&) {
      return wuwe::agent::mcp::mcp_prompt_result {
        .messages = {
          wuwe::agent::mcp::mcp_prompt_message::user_content(
            wuwe::agent::mcp::mcp_content::image("iVBORw0KGgo=")),
        },
      };
    });

  const auto response = parse_response(server.handle_message(R"({
    "jsonrpc":"2.0",
    "id":21,
    "method":"prompts/get",
    "params":{"name":"show_image","arguments":{}}
  })"));

  const auto content = response["result"]["messages"][0]["content"];
  require(content["type"] == "image", "prompt messages should support image content");
  require(content["mimeType"] == "image/png", "prompt image content should include mime type");
}

void test_unknown_resource_and_prompt_return_jsonrpc_errors() {
  wuwe::agent::mcp::mcp_server server;

  const auto resource_response = parse_response(server.handle_message(R"({
    "jsonrpc":"2.0",
    "id":10,
    "method":"resources/read",
    "params":{"uri":"wuwe://missing"}
  })"));
  require(resource_response["error"]["code"] == -32602,
    "unknown resources should return invalid params");

  const auto prompt_response = parse_response(server.handle_message(R"({
    "jsonrpc":"2.0",
    "id":11,
    "method":"prompts/get",
    "params":{"name":"missing","arguments":{}}
  })"));
  require(prompt_response["error"]["code"] == -32602,
    "unknown prompts should return invalid params");
}

void test_access_policy_filters_and_denies_tools_with_audit() {
  wuwe::tool_provider<echo_text> provider;
  wuwe::agent::mcp::mcp_server server;
  server.add_tool_provider(provider);
  server.set_access_policy({
    .denied_tools = { "echo_text" },
  });

  std::vector<wuwe::agent::mcp::mcp_audit_event> events;
  server.set_audit_sink([&events](const auto& event) {
    events.push_back(event);
  });

  const auto list_response = parse_response(server.handle_message(R"({
    "jsonrpc":"2.0",
    "id":12,
    "method":"tools/list"
  })"));
  require(list_response["result"]["tools"].empty(),
    "denied tools should be hidden from tools/list");

  const auto call_response = parse_response(server.handle_message(R"({
    "jsonrpc":"2.0",
    "id":13,
    "method":"tools/call",
    "params":{"name":"echo_text","arguments":{"text":"blocked"}}
  })"));
  require(call_response["error"]["code"] == -32001,
    "denied tool calls should return request_denied");
  require(events.size() == 1, "denied tool call should emit one audit event");
  require(events[0].action == "tools/call", "audit event should record action");
  require(events[0].target == "echo_text", "audit event should record target");
  require(!events[0].allowed, "audit event should record denied decision");
  require(events[0].arguments["text"] == "blocked", "audit event should include arguments");
}

void test_audit_events_include_scope_and_redacted_arguments() {
  wuwe::agent::mcp::mcp_server server;
  server.set_auth_context({
    .subject = "user-123",
    .issuer = "issuer-a",
    .audience = "wuwe-mcp",
    .scopes = { "tools:call" },
    .claims = { { "email", "user@example.test" } },
  });
  server.add_mcp_tool(
    {
      .name = "store_secret",
      .description = "Store secret-shaped inputs.",
      .parameters_json_schema = R"({"type":"object","properties":{}})",
    },
    [](const wuwe::agent::mcp::json&) {
      return wuwe::agent::mcp::mcp_tool_call_result {
        .content = { wuwe::agent::mcp::mcp_content::text_content("ok") },
      };
    });
  server.set_access_policy({
    .tenant_id = "tenant-a",
    .scopes = { "tools:call", "audit:write" },
    .allowed_tools = { "store_secret" },
    .redacted_argument_keys = { "token", "password", "nested_secret" },
  });

  std::vector<wuwe::agent::mcp::mcp_audit_event> events;
  server.set_audit_sink([&events](const auto& event) {
    events.push_back(event);
  });

  const auto response = parse_response(server.handle_message(R"({
    "jsonrpc":"2.0",
    "id":14,
    "method":"tools/call",
    "params":{
      "name":"store_secret",
      "arguments":{
        "token":"open-sesame",
        "safe":"visible",
        "nested":{"nested_secret":"hidden"},
        "items":[{"password":"p1"}]
      }
    }
  })"));
  require(response["result"]["content"][0]["text"] == "ok",
    "allowed audited tool call should still execute");
  require(events.size() == 1, "allowed tool call should emit one audit event");
  require(events[0].tenant_id == "tenant-a", "audit event should include tenant id");
  require(events[0].auth.subject == "user-123", "audit event should include auth subject");
  require(events[0].auth.claims["email"] == "user@example.test",
    "audit event should include auth claims");
  require(events[0].scopes.size() == 2, "audit event should include scopes");
  require(events[0].redacted, "audit event should flag redacted arguments");
  require(events[0].arguments["token"] == "[redacted]",
    "audit event should redact top-level secret keys");
  require(events[0].arguments["safe"] == "visible",
    "audit event should preserve non-sensitive arguments");
  require(events[0].arguments["nested"]["nested_secret"] == "[redacted]",
    "audit event should redact nested secret keys");
  require(events[0].arguments["items"][0]["password"] == "[redacted]",
    "audit event should redact array object secret keys");
}

void test_access_policy_allowlist_filters_resources_templates_and_prompts() {
  wuwe::agent::mcp::mcp_server server;
  server.add_resource(
    { .uri = "wuwe://allowed", .name = "Allowed" },
    [] {
      return std::vector<wuwe::agent::mcp::mcp_resource_content> {
        { .uri = "wuwe://allowed", .text = "allowed resource" },
      };
    });
  server.add_resource(
    { .uri = "wuwe://blocked", .name = "Blocked" },
    [] {
      return std::vector<wuwe::agent::mcp::mcp_resource_content> {
        { .uri = "wuwe://blocked", .text = "blocked resource" },
      };
    });
  server.add_resource_template({ .uri_template = "wuwe://allowed/{id}", .name = "Allowed" });
  server.add_resource_template({ .uri_template = "wuwe://blocked/{id}", .name = "Blocked" });
  server.add_prompt(
    { .name = "allowed_prompt" },
    [](const json&) {
      return wuwe::agent::mcp::mcp_prompt_result {
        .messages = { { .role = "user", .text = "allowed" } },
      };
    });
  server.add_prompt(
    { .name = "blocked_prompt" },
    [](const json&) {
      return wuwe::agent::mcp::mcp_prompt_result {
        .messages = { { .role = "user", .text = "blocked" } },
      };
    });
  server.set_access_policy({
    .allowed_resources = { "wuwe://allowed" },
    .allowed_resource_templates = { "wuwe://allowed/{id}" },
    .allowed_prompts = { "allowed_prompt" },
  });

  const auto resources_response = parse_response(server.handle_message(R"({
    "jsonrpc":"2.0",
    "id":14,
    "method":"resources/list"
  })"));
  require(resources_response["result"]["resources"].size() == 1,
    "resource allowlist should filter resources/list");
  require(resources_response["result"]["resources"][0]["uri"] == "wuwe://allowed",
    "resource allowlist should keep allowed resource");

  const auto templates_response = parse_response(server.handle_message(R"({
    "jsonrpc":"2.0",
    "id":15,
    "method":"resources/templates/list"
  })"));
  require(templates_response["result"]["resourceTemplates"].size() == 1,
    "resource template allowlist should filter templates/list");

  const auto prompts_response = parse_response(server.handle_message(R"({
    "jsonrpc":"2.0",
    "id":16,
    "method":"prompts/list"
  })"));
  require(prompts_response["result"]["prompts"].size() == 1,
    "prompt allowlist should filter prompts/list");
  require(prompts_response["result"]["prompts"][0]["name"] == "allowed_prompt",
    "prompt allowlist should keep allowed prompt");

  const auto denied_resource = parse_response(server.handle_message(R"({
    "jsonrpc":"2.0",
    "id":17,
    "method":"resources/read",
    "params":{"uri":"wuwe://blocked"}
  })"));
  require(denied_resource["error"]["code"] == -32001,
    "resource allowlist should deny unlisted resource reads");

  const auto allowed_prompt = parse_response(server.handle_message(R"({
    "jsonrpc":"2.0",
    "id":18,
    "method":"prompts/get",
    "params":{"name":"allowed_prompt","arguments":{}}
  })"));
  require(allowed_prompt["result"]["messages"][0]["content"]["text"] == "allowed",
    "prompt allowlist should allow listed prompt");
}

void test_roots_list_supports_pagination_and_client_helper() {
  wuwe::agent::mcp::mcp_server server;
  server.set_list_page_size(1);
  server.add_root({ .uri = "file:///workspace/a", .name = "Workspace A" });
  server.add_root({ .uri = "file:///workspace/b", .name = "Workspace B" });

  const auto first_page = parse_response(server.handle_message(R"({
    "jsonrpc":"2.0",
    "id":55,
    "method":"roots/list"
  })"));
  require(first_page["result"]["roots"].size() == 1,
    "roots/list should support pagination");
  require(first_page["result"]["roots"][0]["uri"] == "file:///workspace/a",
    "roots/list should include root uri");
  require(first_page["result"].contains("nextCursor"),
    "roots/list should return nextCursor when more roots exist");

  const auto second_page = parse_response(server.handle_message(R"({
    "jsonrpc":"2.0",
    "id":56,
    "method":"roots/list",
    "params":{"cursor":"1"}
  })"));
  require(second_page["result"]["roots"][0]["name"] == "Workspace B",
    "roots/list should page by cursor");
  require(server.roots().size() == 2, "server should expose registered roots");

  std::ostringstream framed_input;
  wuwe::agent::mcp::mcp_stdio_transport::write_framed_message(framed_input,
    R"({"jsonrpc":"2.0","id":1,"result":{"roots":[{"uri":"file:///client","name":"Client Root"}]}})");
  std::istringstream input(framed_input.str());
  std::ostringstream output;
  wuwe::agent::mcp::mcp_stdio_client client(input, output);
  const auto client_response = client.list_roots();
  require(client_response["result"]["roots"][0]["name"] == "Client Root",
    "stdio client should support roots/list");

  std::istringstream sent(output.str());
  const auto request = wuwe::agent::mcp::mcp_stdio_transport::read_framed_message(sent);
  require(request.has_value(), "stdio client should write roots/list request");
  require(json::parse(*request)["method"] == "roots/list",
    "stdio client roots helper should send roots/list");
}

void test_async_task_registry_tracks_completion_progress_cancel_and_timeout() {
  wuwe::agent::mcp::mcp_async_task_registry registry;
  registry.submit(
    "async-complete",
    "tools/call",
    "worker",
    json::object(),
    [&registry](wuwe::agent::mcp::mcp_async_cancel_token token) {
      registry.progress("async-complete", "token-1", 50.0, 100.0, "halfway");
      require(!token.is_cancelled(), "new async task should not start cancelled");
    });

  for (int i = 0; i < 50; ++i) {
    const auto snapshot = registry.poll("async-complete");
    if (snapshot && snapshot->ready) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  const auto completed = registry.poll("async-complete");
  require(completed.has_value(), "async registry should return submitted task");
  require(completed->record.state == wuwe::agent::mcp::mcp_request_state::completed,
    "async registry should mark finished task completed");
  require(completed->record.progress == 50.0,
    "async registry should store task progress");
  require(completed->record.progress_message == "halfway",
    "async registry should store task progress message");

  registry.submit(
    "async-cancel",
    "tools/call",
    "worker",
    json::object(),
    [](wuwe::agent::mcp::mcp_async_cancel_token token) {
      for (int i = 0; i < 50 && !token.is_cancelled(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    });
  require(registry.cancel("async-cancel", "stop requested"),
    "async registry should cancel known task");
  const auto cancelled = registry.poll("async-cancel");
  require(cancelled->record.state == wuwe::agent::mcp::mcp_request_state::cancelled,
    "async registry should mark cancellation");
  require(cancelled->record.error == "stop requested",
    "async registry should store cancellation reason");

  registry.submit(
    "async-timeout",
    "tools/call",
    "worker",
    json::object(),
    [](wuwe::agent::mcp::mcp_async_cancel_token token) {
      for (int i = 0; i < 50 && !token.is_cancelled(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    },
    std::chrono::milliseconds(1));
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  const auto timed_out = registry.poll("async-timeout");
  require(timed_out->record.state == wuwe::agent::mcp::mcp_request_state::failed,
    "async registry should mark timed out task failed");
  require(timed_out->record.error == "request timed out",
    "async registry should record timeout error");
}

void test_stdio_transport_uses_content_length_framing() {
  wuwe::tool_provider<echo_text> provider;
  wuwe::agent::mcp::mcp_server server;
  server.add_tool_provider(provider);

  std::ostringstream framed_input;
  wuwe::agent::mcp::mcp_stdio_transport::write_framed_message(framed_input,
    R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"echo_text","arguments":{"text":"one"}}})");
  wuwe::agent::mcp::mcp_stdio_transport::write_framed_message(framed_input,
    R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
  wuwe::agent::mcp::mcp_stdio_transport::write_framed_message(framed_input,
    R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})");

  std::istringstream input(framed_input.str());
  std::ostringstream output;

  wuwe::agent::mcp::mcp_stdio_transport transport;
  require(transport.run(server, input, output) == 0, "stdio transport should finish cleanly");

  const auto text = output.str();
  require(text.find("Content-Length: ") != std::string::npos,
    "stdio should write Content-Length framed responses");
  require(text.find("\"id\":1") != std::string::npos, "stdio should write call response");
  require(text.find("\"id\":2") != std::string::npos, "stdio should write list response");
  require(text.find("notifications/initialized") == std::string::npos,
    "stdio should not write notification responses");
}

void test_stdio_transport_supports_json_lines_framing() {
  wuwe::tool_provider<echo_text> provider;
  wuwe::agent::mcp::mcp_server server({ .name = "json-lines-server", .version = "1.0.0" });
  server.add_tool_provider(provider);
  server.add_resource(
    { .uri = "wuwe://json-lines/readme", .name = "JSON Lines README" },
    [] {
      return std::vector<wuwe::agent::mcp::mcp_resource_content> {
        wuwe::agent::mcp::mcp_resource_content::text_content(
          "wuwe://json-lines/readme", "json-lines resource"),
      };
    });

  std::istringstream input(
    R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","clientInfo":{"name":"codex-mcp-client","version":"0.128.0-alpha.1"},"capabilities":{"elicitation":{"form":{}}}}})"
    "\n"
    R"({"jsonrpc":"2.0","method":"notifications/initialized"})"
    "\n"
    R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})"
    "\n"
    R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"echo_text","arguments":{"text":"json lines"}}})"
    "\n"
    R"({"jsonrpc":"2.0","id":4,"method":"resources/read","params":{"uri":"wuwe://json-lines/readme"}})"
    "\n");
  std::ostringstream output;

  wuwe::agent::mcp::mcp_stdio_transport transport;
  require(transport.run(server, input, output) == 0,
    "stdio transport should accept line-delimited JSON-RPC from hosts");

  std::istringstream responses(output.str());
  std::string line;
  std::vector<json> parsed;
  while (std::getline(responses, line)) {
    if (!line.empty()) {
      parsed.push_back(json::parse(line));
    }
  }

  require(parsed.size() == 4,
    "JSON Lines stdio should return one line per request with a response");
  require(parsed[0]["result"]["serverInfo"]["name"] == "json-lines-server",
    "JSON Lines stdio should initialize server");
  require(parsed[1]["result"]["tools"].size() == 1,
    "JSON Lines stdio should list tools");
  require(parsed[2]["result"]["content"][0]["text"] == "json lines",
    "JSON Lines stdio should call tools");
  require(parsed[3]["result"]["contents"][0]["text"] == "json-lines resource",
    "JSON Lines stdio should read resources");
  require(output.str().find("Content-Length") == std::string::npos,
    "JSON Lines stdio should respond using line-delimited JSON");
}
void test_stdio_transport_writes_notifications_before_response() {
  wuwe::agent::mcp::mcp_server server;
  server.add_mcp_tool(
    {
      .name = "logged_task",
      .description = "Emit one log notification.",
      .parameters_json_schema = R"({"type":"object","properties":{},"additionalProperties":false})",
    },
    [&server](const json&) {
      server.emit_log("info", "logged task ran");
      return wuwe::agent::mcp::mcp_tool_call_result {
        .content = { { .type = "text", .text = "ok" } },
      };
    });

  std::ostringstream framed_input;
  wuwe::agent::mcp::mcp_stdio_transport::write_framed_message(framed_input,
    R"({"jsonrpc":"2.0","id":23,"method":"tools/call","params":{"name":"logged_task","arguments":{}}})");
  std::istringstream input(framed_input.str());
  std::ostringstream output;

  wuwe::agent::mcp::mcp_stdio_transport transport;
  require(transport.run(server, input, output) == 0, "stdio transport should finish cleanly");
  const auto text = output.str();
  require(text.find("notifications/message") != std::string::npos,
    "stdio transport should write emitted notifications");
  require(text.find("\"id\":23") != std::string::npos,
    "stdio transport should still write final response");
  require(text.find("notifications/message") < text.find("\"id\":23"),
    "notifications should be written before final response");
}

void test_stdio_transport_writes_client_requests_before_response() {
  wuwe::agent::mcp::mcp_server server;
  server.add_mcp_tool(
    { .name = "sampling_task", .description = "Request host sampling." },
    [&server](const json&) {
      server.request_sampling(json { { "messages", json::array() } });
      return wuwe::agent::mcp::mcp_tool_call_result::text("queued");
    });

  std::ostringstream framed_input;
  wuwe::agent::mcp::mcp_stdio_transport::write_framed_message(framed_input,
    R"({"jsonrpc":"2.0","id":59,"method":"tools/call","params":{"name":"sampling_task","arguments":{}}})");
  std::istringstream input(framed_input.str());
  std::ostringstream output;

  wuwe::agent::mcp::mcp_stdio_transport transport;
  require(transport.run(server, input, output) == 0, "stdio transport should finish cleanly");

  std::istringstream response_stream(output.str());
  const auto outbound_request =
    wuwe::agent::mcp::mcp_stdio_transport::read_framed_message(response_stream);
  const auto final_response =
    wuwe::agent::mcp::mcp_stdio_transport::read_framed_message(response_stream);
  require(outbound_request.has_value(), "stdio should write outbound client request");
  require(final_response.has_value(), "stdio should write final server response");
  require(json::parse(*outbound_request)["method"] == "sampling/createMessage",
    "stdio should frame outbound sampling request");
  require(json::parse(*final_response)["id"] == 59,
    "stdio should write original response after outbound request");
}

void test_stdio_transport_host_compatibility_transcript() {
  wuwe::tool_provider<echo_text> tools;
  wuwe::agent::mcp::mcp_server server({ .name = "compat-server", .version = "1.0.0" });
  server.add_tool_provider(tools);
  server.add_mcp_tool(
    {
      .name = "preview_image",
      .description = "Return image content.",
      .parameters_json_schema = R"({"type":"object","properties":{},"additionalProperties":false})",
    },
    [](const json&) {
      return wuwe::agent::mcp::mcp_tool_call_result {
        .content = { wuwe::agent::mcp::mcp_content::image("iVBORw0KGgo=") },
      };
    });
  server.add_resource(
    { .uri = "wuwe://example/readme", .name = "Example README" },
    [] {
      return std::vector<wuwe::agent::mcp::mcp_resource_content> {
        wuwe::agent::mcp::mcp_resource_content::text_content(
          "wuwe://example/readme", "compat resource"),
      };
    });
  server.add_prompt(
    { .name = "echo_prompt" },
    [](const json& arguments) {
      return wuwe::agent::mcp::mcp_prompt_result::single_user_message(
        "Echo this topic: " + arguments.value("topic", std::string("compat")));
    });

  std::ostringstream framed_input;
  wuwe::agent::mcp::mcp_stdio_transport::write_framed_message(framed_input,
    R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","clientInfo":{"name":"compat-host","version":"0.1.0"},"capabilities":{}}})");
  wuwe::agent::mcp::mcp_stdio_transport::write_framed_message(framed_input,
    R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
  wuwe::agent::mcp::mcp_stdio_transport::write_framed_message(framed_input,
    R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})");
  wuwe::agent::mcp::mcp_stdio_transport::write_framed_message(framed_input,
    R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"echo_text","arguments":{"text":"hello host"}}})");
  wuwe::agent::mcp::mcp_stdio_transport::write_framed_message(framed_input,
    R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"preview_image","arguments":{}}})");
  wuwe::agent::mcp::mcp_stdio_transport::write_framed_message(framed_input,
    R"({"jsonrpc":"2.0","id":5,"method":"resources/read","params":{"uri":"wuwe://example/readme"}})");
  wuwe::agent::mcp::mcp_stdio_transport::write_framed_message(framed_input,
    R"({"jsonrpc":"2.0","id":6,"method":"prompts/get","params":{"name":"echo_prompt","arguments":{"topic":"host compatibility"}}})");

  std::istringstream input(framed_input.str());
  std::ostringstream output;
  wuwe::agent::mcp::mcp_stdio_transport transport;
  require(transport.run(server, input, output) == 0,
    "compat transcript should run to completion");

  std::istringstream response_stream(output.str());
  std::vector<json> responses;
  while (const auto frame =
           wuwe::agent::mcp::mcp_stdio_transport::read_framed_message(response_stream)) {
    responses.push_back(json::parse(*frame));
  }

  require(responses.size() == 6, "compat transcript should return six responses");
  require(responses[0]["result"]["serverInfo"]["name"] == "compat-server",
    "compat transcript should initialize server");
  require(responses[1]["result"]["tools"].size() == 2,
    "compat transcript should list both tools");
  require(responses[2]["result"]["content"][0]["text"] == "hello host",
    "compat transcript should call echo_text");
  require(responses[3]["result"]["content"][0]["type"] == "image",
    "compat transcript should call preview_image");
  require(responses[4]["result"]["contents"][0]["text"] == "compat resource",
    "compat transcript should read resource");
  require(responses[5]["result"]["messages"][0]["content"]["text"].get<std::string>().find(
            "host compatibility") != std::string::npos,
    "compat transcript should get prompt");
}

void test_stdio_transport_line_mode_is_available_for_debugging() {
  wuwe::tool_provider<echo_text> provider;
  wuwe::agent::mcp::mcp_server server;
  server.add_tool_provider(provider);

  std::istringstream input(
    R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"echo_text","arguments":{"text":"one"}}})"
    "\n");
  std::ostringstream output;

  wuwe::agent::mcp::mcp_stdio_transport transport;
  require(transport.run_lines(server, input, output) == 0,
    "line-delimited debug transport should finish cleanly");
  require(output.str().find("\"text\":\"one\"") != std::string::npos,
    "line-delimited debug transport should write JSON responses");
}

void test_stdio_framing_parser_handles_case_and_whitespace() {
  std::istringstream input("content-length: 7\r\nX-Test: ignored\r\n\r\n{\"a\":1}");

  const auto message = wuwe::agent::mcp::mcp_stdio_transport::read_framed_message(input);
  require(message.has_value(), "framing parser should read valid Content-Length messages");
  require(*message == R"({"a":1})", "framing parser should return the exact JSON body");
}

void test_stdio_framing_parser_rejects_invalid_frames() {
  {
    std::istringstream input("Content-Length: nope\r\n\r\n{}");
    const auto message = wuwe::agent::mcp::mcp_stdio_transport::read_framed_message(input);
    require(!message.has_value(), "framing parser should reject non-numeric content length");
  }
  {
    std::istringstream input("Content-Length: 12\r\n\r\n{\"a\":1}");
    const auto message = wuwe::agent::mcp::mcp_stdio_transport::read_framed_message(input);
    require(!message.has_value(), "framing parser should reject truncated bodies");
  }
  {
    std::istringstream input("X-Test: ignored\r\n\r\n{}");
    const auto message = wuwe::agent::mcp::mcp_stdio_transport::read_framed_message(input);
    require(!message.has_value(), "framing parser should require Content-Length");
  }
}

void test_large_tool_payload_round_trips_through_server_and_stdio() {
  wuwe::tool_provider<echo_text> provider;
  wuwe::agent::mcp::mcp_server server;
  server.add_tool_provider(provider);

  const std::string large_text(128 * 1024, 'x');
  const json request {
    { "jsonrpc", "2.0" },
    { "id", 54 },
    { "method", "tools/call" },
    { "params", {
      { "name", "echo_text" },
      { "arguments", { { "text", large_text } } },
    } },
  };

  const auto direct_response = parse_response(server.handle_message(request.dump()));
  require(direct_response["result"]["content"][0]["text"] == large_text,
    "server should round-trip large tool arguments");

  std::ostringstream framed_input;
  wuwe::agent::mcp::mcp_stdio_transport::write_framed_message(framed_input, request.dump());
  std::istringstream input(framed_input.str());
  std::ostringstream output;
  wuwe::agent::mcp::mcp_stdio_transport transport;
  require(transport.run(server, input, output) == 0,
    "stdio transport should handle a large framed request");

  std::istringstream response_stream(output.str());
  const auto framed_response =
    wuwe::agent::mcp::mcp_stdio_transport::read_framed_message(response_stream);
  require(framed_response.has_value(), "stdio transport should write a large framed response");
  require(json::parse(*framed_response)["result"]["content"][0]["text"] == large_text,
    "stdio transport should round-trip large payload content");
}

void test_stdio_client_sends_requests_and_collects_notifications() {
  std::ostringstream framed_input;
  wuwe::agent::mcp::mcp_stdio_transport::write_framed_message(framed_input,
    R"({"jsonrpc":"2.0","method":"notifications/message","params":{"level":"info","data":{"message":"hello"}}})");
  wuwe::agent::mcp::mcp_stdio_transport::write_framed_message(framed_input,
    R"({"jsonrpc":"2.0","id":1,"result":{"tools":[]}})");

  std::istringstream input(framed_input.str());
  std::ostringstream output;
  wuwe::agent::mcp::mcp_stdio_client client(input, output);

  const auto response = client.list_tools();

  require(response["result"]["tools"].empty(), "stdio client should read matching response");
  require(client.notifications().size() == 1, "stdio client should collect notifications");
  require(client.notifications()[0]["method"] == "notifications/message",
    "stdio client should store notification messages");

  std::istringstream sent(output.str());
  const auto request = wuwe::agent::mcp::mcp_stdio_transport::read_framed_message(sent);
  require(request.has_value(), "stdio client should write framed requests");
  const auto request_json = json::parse(*request);
  require(request_json["method"] == "tools/list", "stdio client should send requested method");
}

void test_stdio_client_sends_notifications_without_waiting() {
  std::istringstream input;
  std::ostringstream output;
  wuwe::agent::mcp::mcp_stdio_client client(input, output);

  client.notify("notifications/initialized");

  std::istringstream sent(output.str());
  const auto notification = wuwe::agent::mcp::mcp_stdio_transport::read_framed_message(sent);
  require(notification.has_value(), "stdio client should write framed notifications");
  const auto notification_json = json::parse(*notification);
  require(!notification_json.contains("id"), "stdio client notifications should not contain id");
  require(notification_json["method"] == "notifications/initialized",
    "stdio client should send notification method");
}

void test_stdio_client_has_resource_prompt_and_ping_helpers() {
  std::ostringstream framed_input;
  wuwe::agent::mcp::mcp_stdio_transport::write_framed_message(framed_input,
    R"({"jsonrpc":"2.0","id":1,"result":{}})");
  wuwe::agent::mcp::mcp_stdio_transport::write_framed_message(framed_input,
    R"({"jsonrpc":"2.0","id":2,"result":{"resources":[]}})");
  wuwe::agent::mcp::mcp_stdio_transport::write_framed_message(framed_input,
    R"({"jsonrpc":"2.0","id":3,"result":{"contents":[{"uri":"wuwe://doc","text":"hello"}]}})");
  wuwe::agent::mcp::mcp_stdio_transport::write_framed_message(framed_input,
    R"({"jsonrpc":"2.0","id":4,"result":{}})");
  wuwe::agent::mcp::mcp_stdio_transport::write_framed_message(framed_input,
    R"({"jsonrpc":"2.0","id":5,"result":{}})");
  wuwe::agent::mcp::mcp_stdio_transport::write_framed_message(framed_input,
    R"({"jsonrpc":"2.0","id":6,"result":{"resourceTemplates":[]}})");
  wuwe::agent::mcp::mcp_stdio_transport::write_framed_message(framed_input,
    R"({"jsonrpc":"2.0","id":7,"result":{"prompts":[]}})");
  wuwe::agent::mcp::mcp_stdio_transport::write_framed_message(framed_input,
    R"({"jsonrpc":"2.0","id":8,"result":{"messages":[]}})");

  std::istringstream input(framed_input.str());
  std::ostringstream output;
  wuwe::agent::mcp::mcp_stdio_client client(input, output);

  require(client.ping()["result"].is_object(), "stdio client should support ping");
  require(client.list_resources()["result"]["resources"].empty(),
    "stdio client should support resources/list");
  require(client.read_resource("wuwe://doc")["result"]["contents"][0]["text"] == "hello",
    "stdio client should support resources/read");
  require(client.subscribe_resource("wuwe://doc")["result"].is_object(),
    "stdio client should support resources/subscribe");
  require(client.unsubscribe_resource("wuwe://doc")["result"].is_object(),
    "stdio client should support resources/unsubscribe");
  require(client.list_resource_templates()["result"]["resourceTemplates"].empty(),
    "stdio client should support resources/templates/list");
  require(client.list_prompts()["result"]["prompts"].empty(),
    "stdio client should support prompts/list");
  require(client.get_prompt("echo")["result"]["messages"].empty(),
    "stdio client should support prompts/get");

  std::istringstream sent(output.str());
  std::vector<std::string> methods;
  while (const auto request = wuwe::agent::mcp::mcp_stdio_transport::read_framed_message(sent)) {
    methods.push_back(json::parse(*request)["method"].get<std::string>());
  }
  require(methods.size() == 8, "stdio client should write one request per helper call");
  require(methods[0] == "ping", "first helper should send ping");
  require(methods[7] == "prompts/get", "last helper should send prompts/get");
}

void test_process_client_launches_stdio_server() {
#ifdef WUWE_MCP_STDIO_EXAMPLE_PATH
  wuwe::agent::mcp::mcp_process_client client({
    .command = WUWE_MCP_STDIO_EXAMPLE_PATH,
  });

  const auto initialize = client.initialize({ .name = "wuwe-process-test", .version = "1.0" });
  require(initialize["result"]["serverInfo"]["name"] == "wuwe-mcp-example",
    "process client should initialize a launched stdio server");

  client.notify("notifications/initialized");
  require(client.ping()["result"].is_object(), "process client should send ping");

  const auto tools = client.list_tools();
  require(tools["result"]["tools"].size() >= 2,
    "process client should list tools from launched server");

  const auto result = client.call_tool("echo_text", { { "text", "hello process" } });
  require(result["result"]["content"][0]["text"] == "hello process",
    "process client should call tools on launched server");

  require(client.list_resource_templates()["result"]["resourceTemplates"].size() == 1,
    "process client should list resource templates");
  require(client.list_roots()["result"]["roots"].is_array(),
    "process client should list roots");
  require(client.list_prompts()["result"]["prompts"].size() == 1,
    "process client should list prompts");

  client.stop();
  require(!client.running(), "process client should stop launched server");
#endif
}

void test_process_client_captures_stderr() {
  wuwe::agent::mcp::mcp_process_client client;
#ifdef _WIN32
  client.start({
    .command = "cmd.exe",
    .args = { "/C", "echo process-stderr 1>&2" },
  });
#else
  client.start({
    .command = "sh",
    .args = { "-c", "echo process-stderr >&2" },
  });
#endif
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  client.stop();
  require(client.stderr_output().find("process-stderr") != std::string::npos,
    "process client should capture child stderr");
  client.clear_stderr_output();
  require(client.stderr_output().empty(),
    "process client should clear captured stderr");
}

void test_process_client_passes_environment_to_child() {
  wuwe::agent::mcp::mcp_process_client client;
#ifdef _WIN32
  client.start({
    .command = "cmd.exe",
    .args = { "/C", "echo %WUWE_MCP_ENV_TEST% 1>&2" },
    .environment = { { "WUWE_MCP_ENV_TEST", "env-value" } },
  });
#else
  client.start({
    .command = "sh",
    .args = { "-c", "printf '%s' \"$WUWE_MCP_ENV_TEST\" >&2" },
    .environment = { { "WUWE_MCP_ENV_TEST", "env-value" } },
  });
#endif
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  client.stop();
  require(client.stderr_output().find("env-value") != std::string::npos,
    "process client should pass environment overrides to child process");
}

void test_host_runtime_manages_multiple_process_servers() {
#ifdef WUWE_MCP_STDIO_EXAMPLE_PATH
  wuwe::agent::mcp::mcp_host_runtime runtime;
  runtime.add_server({
    .id = "alpha",
    .command = { .command = WUWE_MCP_STDIO_EXAMPLE_PATH },
    .client_info = { .name = "wuwe-runtime-test", .version = "1.0" },
  });
  runtime.add_server({
    .id = "beta",
    .command = { .command = WUWE_MCP_STDIO_EXAMPLE_PATH },
    .client_info = { .name = "wuwe-runtime-test", .version = "1.0" },
  });

  require(runtime.contains("alpha") && runtime.contains("beta"),
    "host runtime should track configured servers");

  runtime.start_all();
  require(runtime.snapshot("alpha").state == wuwe::agent::mcp::mcp_host_server_state::running,
    "host runtime should mark started servers running");
  require(runtime.snapshot("beta").running,
    "host runtime snapshot should report live child process");

  const auto alpha = runtime.call_tool("alpha", "echo_text", { { "text", "from alpha" } });
  const auto beta = runtime.call_tool("beta", "echo_text", { { "text", "from beta" } });
  require(alpha["result"]["content"][0]["text"] == "from alpha",
    "host runtime should route tool calls to alpha");
  require(beta["result"]["content"][0]["text"] == "from beta",
    "host runtime should route tool calls to beta");
  require(runtime.list_resources("alpha")["result"]["resources"].size() == 1,
    "host runtime should expose resource helpers");
  require(runtime.list_prompts("beta")["result"]["prompts"].size() == 1,
    "host runtime should expose prompt helpers");
  require(runtime.snapshots().size() == 2,
    "host runtime should expose all server snapshots");
  require(runtime.snapshot("alpha").request_count > 0,
    "host runtime should count routed requests");

  runtime.stop_server("alpha");
  require(runtime.snapshot("alpha").state == wuwe::agent::mcp::mcp_host_server_state::stopped,
    "host runtime should stop one server");
  require(runtime.snapshot("beta").running,
    "host runtime should leave other servers running");

  runtime.stop_all();
  require(!runtime.snapshot("beta").running,
    "host runtime should stop all servers");
#endif
}

void test_host_runtime_dispatches_async_requests() {
#ifdef WUWE_MCP_STDIO_EXAMPLE_PATH
  wuwe::agent::mcp::mcp_host_runtime runtime;
  runtime.add_server({
    .id = "async-a",
    .command = { .command = WUWE_MCP_STDIO_EXAMPLE_PATH },
    .client_info = { .name = "wuwe-async-test", .version = "1.0" },
  });
  runtime.add_server({
    .id = "async-b",
    .command = { .command = WUWE_MCP_STDIO_EXAMPLE_PATH },
    .client_info = { .name = "wuwe-async-test", .version = "1.0" },
  });
  runtime.start_all();

  auto first = runtime.call_tool_async("async-a", "echo_text", { { "text", "async one" } });
  auto second = runtime.call_tool_async("async-b", "echo_text", { { "text", "async two" } });
  auto third = runtime.list_tools_async("async-a");

  require(first.get()["result"]["content"][0]["text"] == "async one",
    "host runtime should dispatch async tool call to first server");
  require(second.get()["result"]["content"][0]["text"] == "async two",
    "host runtime should dispatch async tool call to second server");
  require(!third.get()["result"]["tools"].empty(),
    "host runtime should dispatch async list_tools");
  require(runtime.snapshot("async-a").request_count >= 2,
    "host runtime should count async requests");

  runtime.stop_all();
#endif
}

void test_host_runtime_records_telemetry_events() {
#ifdef WUWE_MCP_STDIO_EXAMPLE_PATH
  wuwe::agent::mcp::mcp_host_runtime runtime;
  std::vector<wuwe::agent::mcp::mcp_host_event> sink_events;
  runtime.set_event_sink([&sink_events](const wuwe::agent::mcp::mcp_host_event& event) {
    sink_events.push_back(event);
  });

  runtime.add_server({
    .id = "telemetry",
    .command = { .command = WUWE_MCP_STDIO_EXAMPLE_PATH },
    .client_info = { .name = "wuwe-telemetry-test", .version = "1.0" },
  });
  runtime.start_server("telemetry");
  runtime.call_tool("telemetry", "echo_text", { { "text", "event stream" } });
  require(runtime.health_check("telemetry"),
    "host runtime telemetry test server should pass health check");
  runtime.stop_server("telemetry");

  const auto events = runtime.events();
  require(events.size() == sink_events.size(),
    "host runtime should send recorded events to sink");
  require(!events.empty() && events.front().sequence == 1,
    "host runtime events should be sequenced");
  require(std::find_if(events.begin(), events.end(), [](const auto& event) {
            return event.type == wuwe::agent::mcp::mcp_host_event_type::server_started &&
                   event.server_id == "telemetry";
          }) != events.end(),
    "host runtime should record server started event");
  require(std::find_if(events.begin(), events.end(), [](const auto& event) {
            return event.type == wuwe::agent::mcp::mcp_host_event_type::request_succeeded &&
                   event.method == "tools/call";
          }) != events.end(),
    "host runtime should record successful request event");
  require(std::find_if(events.begin(), events.end(), [](const auto& event) {
            return event.type == wuwe::agent::mcp::mcp_host_event_type::health_check_succeeded;
          }) != events.end(),
    "host runtime should record health check event");
  require(wuwe::agent::mcp::to_string(
            wuwe::agent::mcp::mcp_host_event_type::request_succeeded) == "request_succeeded",
    "host runtime should stringify event types");

  runtime.clear_events();
  require(runtime.events().empty(), "host runtime should clear telemetry events");
#endif
}

void test_host_runtime_exports_telemetry_events() {
  const auto path = std::filesystem::temp_directory_path() /
                    ("wuwe-mcp-telemetry-" + std::to_string(
                       std::chrono::steady_clock::now().time_since_epoch().count()) + ".jsonl");

  wuwe::agent::mcp::mcp_host_runtime runtime;
  auto memory_sink = std::make_shared<wuwe::agent::mcp::in_memory_mcp_host_event_sink>();
  wuwe::agent::mcp::attach_mcp_host_event_sink(runtime, memory_sink);

  runtime.add_server({
    .id = "telemetry-export",
    .command = { .command = "wuwe-definitely-missing-mcp-server" },
  });

  const auto memory_events = memory_sink->events();
  require(memory_events.size() == 1,
    "host telemetry memory sink should receive runtime events");
  const auto as_json = wuwe::agent::mcp::mcp_host_event_to_json(memory_events.front());
  require(as_json["type"] == "server_added" && as_json["serverId"] == "telemetry-export",
    "host telemetry should serialize event type and server id");
  require(as_json["timestampUnixMillis"].is_number_integer(),
    "host telemetry should serialize event timestamp");

  auto jsonl_sink = std::make_shared<wuwe::agent::mcp::jsonl_mcp_host_event_sink>(path);
  wuwe::agent::mcp::attach_mcp_host_event_sink(runtime, jsonl_sink);
  runtime.remove_server("telemetry-export");

  {
    std::ifstream input(path);
    std::string line;
    std::getline(input, line);
    const auto parsed = json::parse(line);
    require(parsed["type"] == "server_removed" && parsed["serverId"] == "telemetry-export",
      "host telemetry JSONL sink should write one JSON event per line");
  }

  runtime.set_event_sink({});
  jsonl_sink.reset();
  std::filesystem::remove(path);

  wuwe::agent::mcp::prometheus_mcp_host_event_sink prometheus;
  prometheus.publish(memory_events.front());
  require(prometheus.scrape().find("wuwe_mcp_host_events_total") != std::string::npos,
    "host telemetry Prometheus sink should expose event counters");

  wuwe::agent::mcp::otel_mcp_host_event_sink otel;
  otel.publish(memory_events.front());
  require(otel.spans().size() == 1 && otel.spans().front().name == "server_added",
    "host telemetry OTel sink should expose event spans");

  auto fanout = std::make_shared<wuwe::agent::mcp::fanout_mcp_host_event_sink>();
  auto fanout_memory = std::make_shared<wuwe::agent::mcp::in_memory_mcp_host_event_sink>();
  fanout->add_sink(fanout_memory);
  fanout->add_sink(std::make_shared<wuwe::agent::mcp::prometheus_mcp_host_event_sink>());
  fanout->publish(memory_events.front());
  require(fanout_memory->events().size() == 1,
    "host telemetry fanout sink should publish to child sinks");
}

void test_mcp_gateway_aggregates_runtime_servers() {
#ifdef WUWE_MCP_STDIO_EXAMPLE_PATH
  wuwe::agent::mcp::mcp_host_runtime runtime;
  runtime.add_server({
    .id = "gw",
    .command = { .command = WUWE_MCP_STDIO_EXAMPLE_PATH },
    .client_info = { .name = "wuwe-gateway-test", .version = "1.0" },
  });
  runtime.start_all();

  wuwe::agent::mcp::mcp_server gateway_server({
    .name = "gateway-test",
    .version = "1.0",
  });
  wuwe::agent::mcp::mcp_gateway gateway;
  gateway.populate_server(gateway_server, runtime);

  const auto tools_response = json::parse(*gateway_server.handle_message(R"({
    "jsonrpc":"2.0",
    "id":1,
    "method":"tools/list",
    "params":{}
  })"));
  const auto tools = tools_response["result"]["tools"];
  require(std::find_if(tools.begin(), tools.end(), [](const auto& tool) {
            return tool["name"] == "gw__echo_text";
          }) != tools.end(),
    "MCP gateway should expose downstream tools with server prefix");

  const auto call_response = json::parse(*gateway_server.handle_message(R"({
    "jsonrpc":"2.0",
    "id":2,
    "method":"tools/call",
    "params":{"name":"gw__echo_text","arguments":{"text":"through gateway"}}
  })"));
  require(call_response["result"]["content"][0]["text"] == "through gateway",
    "MCP gateway should route tool calls to downstream server");

  const auto resources_response = json::parse(*gateway_server.handle_message(R"({
    "jsonrpc":"2.0",
    "id":3,
    "method":"resources/list",
    "params":{}
  })"));
  require(!resources_response["result"]["resources"].empty(),
    "MCP gateway should expose downstream resources");
  const auto resource_uri = resources_response["result"]["resources"][0]["uri"].get<std::string>();
  const auto read_response = json::parse(*gateway_server.handle_message(json {
    { "jsonrpc", "2.0" },
    { "id", 4 },
    { "method", "resources/read" },
    { "params", { { "uri", resource_uri } } },
  }.dump()));
  require(!read_response["result"]["contents"].empty(),
    "MCP gateway should route resource reads to downstream server");

  const auto prompts_response = json::parse(*gateway_server.handle_message(R"({
    "jsonrpc":"2.0",
    "id":5,
    "method":"prompts/list",
    "params":{}
  })"));
  require(!prompts_response["result"]["prompts"].empty() &&
          prompts_response["result"]["prompts"][0]["name"].get<std::string>().find("gw__") == 0,
    "MCP gateway should expose downstream prompts with server prefix");

  runtime.stop_all();
#endif
}

void test_host_runtime_loads_common_json_config_shapes() {
  const auto vscode_configs = wuwe::agent::mcp::mcp_host_server_configs_from_json(json {
    { "servers", {
      { "wuwe", {
        { "type", "stdio" },
        { "command", "server.exe" },
        { "args", json::array({ "--mode", "test" }) },
        { "cwd", "D:\\workspace" },
        { "env", { { "WUWE_ENV", "enabled" } } },
        { "clientInfo", { { "name", "config-host" }, { "version", "2.0" } } },
        { "capabilities", { { "roots", { { "listChanged", true } } } } },
      } },
    } },
  });
  require(vscode_configs.size() == 1, "host runtime should parse VS Code servers config");
  require(vscode_configs[0].id == "wuwe", "host runtime should use server object key as id");
  require(vscode_configs[0].command.command == "server.exe",
    "host runtime should parse command");
  require(vscode_configs[0].command.args.size() == 2,
    "host runtime should parse args");
  require(vscode_configs[0].command.working_directory == "D:\\workspace",
    "host runtime should parse cwd");
  require(vscode_configs[0].command.environment.at("WUWE_ENV") == "enabled",
    "host runtime should parse env");
  require(vscode_configs[0].client_info.name == "config-host",
    "host runtime should parse clientInfo");
  require(vscode_configs[0].capabilities["roots"]["listChanged"].get<bool>(),
    "host runtime should parse capabilities");

  wuwe::agent::mcp::mcp_host_runtime runtime;
  runtime.add_servers_from_json(json {
    { "mcpServers", {
      { "claude-shape", {
        { "command", "server.exe" },
        { "args", json::array() },
        { "workingDirectory", "D:\\workspace2" },
        { "autoInitialize", false },
        { "restartOnFailure", true },
        { "maxRestarts", 3 },
      } },
    } },
  });
  require(runtime.contains("claude-shape"),
    "host runtime should add Claude/Cursor style mcpServers config");
  require(runtime.snapshot("claude-shape").command.working_directory == "D:\\workspace2",
    "host runtime should parse workingDirectory");
  require(runtime.snapshot("claude-shape").restart_count == 0,
    "host runtime should initialize restart counter");
  const auto configs = wuwe::agent::mcp::mcp_host_server_configs_from_json(json {
    { "mcpServers", {
      { "restart-shape", {
        { "command", "server.exe" },
        { "restartOnFailure", true },
        { "maxRestarts", 3 },
        { "restartBackoffMillis", 25 },
        { "maxRestartBackoffMillis", 100 },
        { "circuitBreakerFailureThreshold", 4 },
        { "circuitBreakerCooldownMillis", 250 },
      } },
    } },
  });
  require(configs[0].restart_on_failure && configs[0].max_restart_attempts == 3,
    "host runtime should parse restart policy");
  require(configs[0].restart_backoff == std::chrono::milliseconds(25) &&
          configs[0].max_restart_backoff == std::chrono::milliseconds(100),
    "host runtime should parse restart backoff policy");
  require(configs[0].circuit_breaker_failure_threshold == 4 &&
          configs[0].circuit_breaker_cooldown == std::chrono::milliseconds(250),
    "host runtime should parse circuit breaker policy");
}

void test_host_runtime_reports_config_diagnostics() {
  const auto diagnostics = wuwe::agent::mcp::mcp_host_config_diagnostics_from_json(json {
    { "servers", {
      { "broken", {
        { "type", "http" },
        { "command", "" },
        { "args", json::array({ "--ok", 42 }) },
        { "env", { { "WUWE_ENV", false } } },
        { "clientInfo", { { "name", 12 } } },
        { "capabilities", json::array() },
        { "autoInitialize", "yes" },
        { "maxRestarts", -1 },
      } },
    } },
  });

  require(diagnostics.size() >= 8,
    "host runtime should report multiple config diagnostics without throwing");
  require(std::any_of(diagnostics.begin(), diagnostics.end(), [](const auto& diagnostic) {
            return diagnostic.path == "$.servers.broken.type" &&
                   diagnostic.severity ==
                     wuwe::agent::mcp::mcp_host_config_diagnostic_severity::error;
          }),
    "host runtime should diagnose unsupported server type");
  require(std::any_of(diagnostics.begin(), diagnostics.end(), [](const auto& diagnostic) {
            return diagnostic.path == "$.servers.broken.command" &&
                   diagnostic.message.find("must not be empty") != std::string::npos;
          }),
    "host runtime should diagnose empty commands");
  require(std::any_of(diagnostics.begin(), diagnostics.end(), [](const auto& diagnostic) {
            return diagnostic.path == "$.servers.broken.args[1]";
          }),
    "host runtime should diagnose non-string args entries");
  require(wuwe::agent::mcp::to_string(
            wuwe::agent::mcp::mcp_host_config_diagnostic_severity::warning) == "warning",
    "host runtime should stringify config diagnostic severity");

  const auto clean = wuwe::agent::mcp::mcp_host_config_diagnostics_from_json(json {
    { "mcpServers", {
      { "wuwe", {
        { "command", "server.exe" },
        { "args", json::array() },
        { "restartOnFailure", true },
        { "maxRestarts", 3 },
      } },
    } },
  });
  require(clean.empty(), "host runtime should not report diagnostics for a valid config");
}

void test_host_runtime_loads_config_files_and_discovers_workspace_paths() {
  const auto root = std::filesystem::temp_directory_path() /
                    ("wuwe-mcp-config-test-" + std::to_string(
                       std::chrono::steady_clock::now().time_since_epoch().count()));
  const auto vscode_dir = root / ".vscode";
  std::filesystem::create_directories(vscode_dir);
  const auto config_path = vscode_dir / "mcp.json";
  {
    std::ofstream config(config_path);
    config << R"({
      "servers": {
        "file-server": {
          "type": "stdio",
          "command": "server.exe",
          "args": ["--from-file"],
          "autoInitialize": false
        }
      }
    })";
  }

  const auto paths = wuwe::agent::mcp::mcp_host_default_config_paths(root);
  require(!paths.empty() && paths.front() == config_path,
    "host runtime should discover workspace VS Code MCP config path first");

  const auto configs = wuwe::agent::mcp::mcp_host_server_configs_from_file(config_path);
  require(configs.size() == 1 && configs[0].id == "file-server",
    "host runtime should parse MCP config files");
  require(configs[0].command.args.size() == 1 && configs[0].command.args[0] == "--from-file",
    "host runtime should parse file config args");

  const auto user_paths = wuwe::agent::mcp::mcp_host_user_config_paths(root, root / "AppData");
#ifdef _WIN32
  require(std::find(user_paths.begin(), user_paths.end(),
            root / "AppData" / "Claude" / "claude_desktop_config.json") != user_paths.end(),
    "host runtime should discover Windows user-level Claude MCP config candidates");
  require(std::find(user_paths.begin(), user_paths.end(),
            root / "AppData" / "Cursor" / "User" / "mcp.json") != user_paths.end(),
    "host runtime should discover Windows user-level Cursor MCP config candidates");
#else
  require(std::find(user_paths.begin(), user_paths.end(),
            root / "Library" / "Application Support" / "Claude" /
              "claude_desktop_config.json") != user_paths.end(),
    "host runtime should discover macOS user-level Claude MCP config candidates");
  require(std::find(user_paths.begin(), user_paths.end(),
            root / ".continue" / "config.json") != user_paths.end(),
    "host runtime should discover home-level MCP config candidates");
#endif

  wuwe::agent::mcp::mcp_host_runtime runtime;
  require(runtime.add_servers_from_file(config_path) == 1,
    "host runtime should add server configs from file");
  require(runtime.contains("file-server"),
    "host runtime should track file-loaded server config");

  std::filesystem::remove_all(root);
}

void test_host_runtime_restarts_failed_process_server() {
#ifdef WUWE_MCP_STDIO_EXAMPLE_PATH
  wuwe::agent::mcp::mcp_host_runtime runtime;
  runtime.add_server({
    .id = "restartable",
    .command = { .command = WUWE_MCP_STDIO_EXAMPLE_PATH },
    .client_info = { .name = "wuwe-restart-test", .version = "1.0" },
    .restart_on_failure = true,
    .max_restart_attempts = 2,
  });

  runtime.start_server("restartable");
  require(runtime.health_check("restartable"),
    "host runtime health check should pass for running server");

  runtime.client("restartable").stop();
  const auto result = runtime.call_tool("restartable", "echo_text", { { "text", "after restart" } });
  require(result["result"]["content"][0]["text"] == "after restart",
    "host runtime should restart failed server and retry request");
  require(runtime.snapshot("restartable").restart_count == 1,
    "host runtime should record restart count");
  require(runtime.snapshot("restartable").failure_count == 1,
    "host runtime should record failed request count");
  require(runtime.snapshot("restartable").request_count >= 1,
    "host runtime should record attempted requests");
  require(runtime.snapshot("restartable").state == wuwe::agent::mcp::mcp_host_server_state::running,
    "host runtime should return to running after restart");

  runtime.client("restartable").stop();
  require(runtime.health_check("restartable"),
    "host runtime health check should restart stopped child when policy allows");
  require(runtime.snapshot("restartable").restart_count == 2,
    "host runtime should record health-check restart");
  require(runtime.snapshot("restartable").health_check_count == 2,
    "host runtime should record health checks");

  runtime.stop_all();
#endif
}

void test_host_runtime_applies_backoff_and_circuit_breaker() {
  wuwe::agent::mcp::mcp_host_runtime runtime;
  runtime.add_server({
    .id = "unstable",
    .command = { .command = "wuwe-definitely-missing-mcp-server" },
    .restart_on_failure = true,
    .max_restart_attempts = 3,
    .restart_backoff = std::chrono::milliseconds(500),
    .max_restart_backoff = std::chrono::milliseconds(500),
    .circuit_breaker_failure_threshold = 2,
    .circuit_breaker_cooldown = std::chrono::milliseconds(1000),
  });

  bool first_failed = false;
  try {
    runtime.start_server("unstable");
  }
  catch (const std::exception&) {
    first_failed = true;
  }
  require(first_failed, "host runtime should surface failed process start");
  auto snapshot = runtime.snapshot("unstable");
  require(snapshot.failure_count == 1 && snapshot.consecutive_failure_count == 1,
    "host runtime should count consecutive failures");
  require(!snapshot.restart_available && snapshot.restart_backoff_remaining.count() > 0,
    "host runtime should block restart while backoff is active");

  bool second_failed = false;
  try {
    runtime.start_server("unstable");
  }
  catch (const std::exception&) {
    second_failed = true;
  }
  require(second_failed, "host runtime should keep surfacing failed starts");
  snapshot = runtime.snapshot("unstable");
  require(snapshot.state == wuwe::agent::mcp::mcp_host_server_state::circuit_open,
    "host runtime should open circuit breaker after repeated failures");
  require(snapshot.circuit_breaker_open && !snapshot.restart_available,
    "host runtime should block restart while circuit breaker is open");
  require(wuwe::agent::mcp::to_string(snapshot.state) == "circuit_open",
    "host runtime should stringify circuit-open state");
  const auto events = runtime.events();
  require(std::find_if(events.begin(), events.end(), [](const auto& event) {
            return event.type == wuwe::agent::mcp::mcp_host_event_type::circuit_opened;
          }) != events.end(),
    "host runtime should record circuit-open events");
}

void test_http_transport_adapts_jsonrpc_requests_and_sse_notifications() {
  wuwe::agent::mcp::mcp_server server;
  server.add_mcp_tool(
    { .name = "http_task", .description = "Emit a log over HTTP." },
    [&server](const json&) {
      server.emit_log("info", "http task ran");
      return wuwe::agent::mcp::mcp_tool_call_result {
        .content = { { .type = "text", .text = "ok" } },
      };
    });

  wuwe::agent::mcp::mcp_http_transport transport;
  const auto response = transport.handle(server, {
    .method = "POST",
    .body = R"({"jsonrpc":"2.0","id":35,"method":"tools/call","params":{"name":"http_task","arguments":{}}})",
  });

  require(response.status_code == 200, "HTTP transport should return 200 for requests");
  require(response.content_type == "application/json", "HTTP transport should expose JSON content type");
  require(json::parse(response.body)["id"] == 35, "HTTP transport should return JSON-RPC body");
  require(!response.sse_body().empty(), "HTTP transport should build a joined SSE body");
  require(response.sse_events.size() == 1,
    "HTTP transport should expose emitted notifications as SSE events");
  require(response.sse_events[0].find("notifications/message") != std::string::npos,
    "HTTP transport SSE event should include notification payload");

  const auto notification_response = transport.handle(server, {
    .method = "POST",
    .body = R"({"jsonrpc":"2.0","method":"notifications/initialized"})",
  });
  require(notification_response.status_code == 202,
    "HTTP transport should return 202 for notification-only requests");

  const auto method_response = transport.handle(server, { .method = "GET" });
  require(method_response.status_code == 405,
    "HTTP transport should reject unsupported methods");
  bool has_allow_header = false;
  for (const auto& header : method_response.headers) {
    has_allow_header = has_allow_header || (header.first == "Allow" && header.second == "POST");
  }
  require(has_allow_header, "HTTP transport should expose an Allow header for 405 responses");

  const auto content_type_response = transport.handle(server, {
    .method = "POST",
    .body = "{}",
    .headers = { { "Content-Type", "text/plain" } },
  });
  require(content_type_response.status_code == 415,
    "HTTP transport should reject non-JSON content types");

  const auto empty_body_response = transport.handle(server, {
    .method = "POST",
    .headers = { { "content-type", "application/json; charset=utf-8" } },
  });
  require(empty_body_response.status_code == 400,
    "HTTP transport should reject empty POST bodies");
}

void test_http_transport_exposes_outbound_client_requests() {
  wuwe::agent::mcp::mcp_server server;
  server.add_mcp_tool(
    { .name = "http_sampling", .description = "Request sampling over HTTP adapter." },
    [&server](const json&) {
      server.request_sampling(json { { "messages", json::array() } });
      return wuwe::agent::mcp::mcp_tool_call_result::text("queued");
    });

  wuwe::agent::mcp::mcp_http_transport transport;
  const auto response = transport.handle(server, {
    .method = "POST",
    .body = R"({"jsonrpc":"2.0","id":60,"method":"tools/call","params":{"name":"http_sampling","arguments":{}}})",
  });

  require(response.status_code == 200,
    "HTTP transport should still return original JSON-RPC response");
  require(response.client_requests.size() == 1,
    "HTTP transport should expose outbound client requests");
  require(json::parse(response.client_requests[0])["method"] == "sampling/createMessage",
    "HTTP transport should expose sampling request payload");
}

void test_http_listener_serves_mcp_and_health_endpoints() {
  wuwe::tool_provider<echo_text> provider;
  wuwe::agent::mcp::mcp_server server;
  server.add_tool_provider(provider);

  wuwe::agent::mcp::mcp_http_listener listener(server);
  require(listener.start(), "HTTP listener should start on localhost");

  httplib::Client client("127.0.0.1", listener.bound_port());
  const auto health = client.Get("/healthz");
  require(health && health->status == 200,
    "HTTP listener should expose a health endpoint");

  const auto response = client.Post(
    "/mcp",
    R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})",
    "application/json");
  require(response && response->status == 200,
    "HTTP listener should serve JSON-RPC requests");
  const auto body = json::parse(response->body);
  require(body["result"]["tools"][0]["name"] == "echo_text",
    "HTTP listener should route requests through the MCP server");

  listener.stop();
}

void test_http_listener_rejects_unauthorized_requests() {
  wuwe::agent::mcp::mcp_server server;
  wuwe::agent::mcp::mcp_http_listener listener(server, {
    .authorize = [](const wuwe::agent::mcp::mcp_http_request& request) {
      for (const auto& [name, value] : request.headers) {
        if (name == "Authorization" && value == "Bearer test-token") {
          return true;
        }
      }
      return false;
    },
  });
  require(listener.start(), "HTTP listener with auth should start on localhost");

  httplib::Client client("127.0.0.1", listener.bound_port());
  const auto rejected = client.Post(
    "/mcp",
    R"({"jsonrpc":"2.0","id":1,"method":"ping"})",
    "application/json");
  require(rejected && rejected->status == 401,
    "HTTP listener should reject unauthorized requests");

  httplib::Headers headers { { "Authorization", "Bearer test-token" } };
  const auto accepted = client.Post(
    "/mcp",
    headers,
    R"({"jsonrpc":"2.0","id":1,"method":"ping"})",
    "application/json");
  require(accepted && accepted->status == 200,
    "HTTP listener should allow authorized requests");

  listener.stop();
}

} // namespace

int main() {
  try {
    test_initialize_reports_server_capabilities();
    test_initialize_records_client_info_and_capabilities();
    test_initialized_notification_sets_state_and_ping_responds();
    test_tools_list_exposes_provider_schema();
    test_list_methods_support_cursor_pagination();
    test_tools_call_invokes_provider();
    test_mcp_tool_can_return_image_content();
    test_invalid_arguments_become_tool_error_result();
    test_invalid_json_and_unknown_tool_return_jsonrpc_errors();
    test_jsonrpc_batch_returns_response_array_and_skips_notifications();
    test_empty_jsonrpc_batch_is_invalid();
    test_batch_reports_malformed_items_and_params();
    test_notifications_do_not_write_responses();
    test_cancelled_notification_tracks_request_id();
    test_tools_can_emit_log_and_progress_notifications();
    test_request_progress_updates_lifecycle_record();
    test_request_timeout_marks_lifecycle_failed();
    test_sampling_and_elicitation_requests_round_trip_through_exchange();
    test_sampling_and_elicitation_follow_client_capabilities();
    test_typed_sampling_and_elicitation_helpers();
    test_resources_list_and_read_registered_content();
    test_resources_read_can_return_blob_content();
    test_resources_subscribe_unsubscribe_and_update_notifications();
    test_resource_templates_list_registered_templates();
    test_prompts_list_and_get_registered_prompt();
    test_prompts_get_can_return_non_text_content();
    test_request_registry_records_failures();
    test_unknown_resource_and_prompt_return_jsonrpc_errors();
    test_access_policy_filters_and_denies_tools_with_audit();
    test_audit_events_include_scope_and_redacted_arguments();
    test_access_policy_allowlist_filters_resources_templates_and_prompts();
    test_roots_list_supports_pagination_and_client_helper();
    test_async_task_registry_tracks_completion_progress_cancel_and_timeout();
    test_stdio_transport_uses_content_length_framing();
    test_stdio_transport_supports_json_lines_framing();
    test_stdio_transport_writes_notifications_before_response();
    test_stdio_transport_writes_client_requests_before_response();
    test_stdio_transport_host_compatibility_transcript();
    test_stdio_transport_line_mode_is_available_for_debugging();
    test_stdio_framing_parser_handles_case_and_whitespace();
    test_stdio_framing_parser_rejects_invalid_frames();
    test_large_tool_payload_round_trips_through_server_and_stdio();
    test_stdio_client_sends_requests_and_collects_notifications();
    test_stdio_client_sends_notifications_without_waiting();
    test_stdio_client_has_resource_prompt_and_ping_helpers();
    test_process_client_launches_stdio_server();
    test_process_client_captures_stderr();
    test_process_client_passes_environment_to_child();
    test_host_runtime_manages_multiple_process_servers();
    test_host_runtime_dispatches_async_requests();
    test_host_runtime_records_telemetry_events();
    test_host_runtime_exports_telemetry_events();
    test_mcp_gateway_aggregates_runtime_servers();
    test_host_runtime_loads_common_json_config_shapes();
    test_host_runtime_reports_config_diagnostics();
    test_host_runtime_loads_config_files_and_discovers_workspace_paths();
    test_host_runtime_restarts_failed_process_server();
    test_host_runtime_applies_backoff_and_circuit_breaker();
    test_http_transport_adapts_jsonrpc_requests_and_sse_notifications();
    test_http_transport_exposes_outbound_client_requests();
    test_http_listener_serves_mcp_and_health_endpoints();
    test_http_listener_rejects_unauthorized_requests();
  }
  catch (const std::exception& ex) {
    wuwe::println("mcp_tests failed: {}", ex.what());
    return 1;
  }

  wuwe::println("mcp_tests passed");
  return 0;
}
