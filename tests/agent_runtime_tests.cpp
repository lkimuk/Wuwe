#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include <wuwe/agent/llm/llm_agent_runner.h>
#include <wuwe/agent/llm/llm_error.h>
#include <wuwe/agent/llm/openrouter_llm_client.h>
#include <wuwe/agent/tools/tool.hpp>
#include <wuwe/common/print.h>
#include <wuwe/net/http_client.h>
#include <wuwe/net/sse_event_parser.h>

namespace agent_runtime_test_tools {

struct echo_text {
  static constexpr std::string_view description = "Echo text back to the caller.";

  std::string text;

  std::string invoke() const {
    return text;
  }
};

struct session_context {
  std::string prefix;
};

struct session_lookup {
  static constexpr std::string_view description = "Look up a value in application-owned state.";

  wuwe::field<std::string> key {
    .description = "Application state key to look up.",
  };

  wuwe::llm_tool_result invoke(const session_context& context) const {
    return { .content = context.prefix + key.value };
  }
};

} // namespace agent_runtime_test_tools

namespace {

using namespace wuwe;
using namespace agent_runtime_test_tools;

void require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

bool contains(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

class tool_call_client final : public llm_client {
public:
  llm_response complete(const llm_request& request) override {
    return complete(request, {});
  }

  llm_response complete(const llm_request& request, std::stop_token stop_token) override {
    ++calls;
    saw_stop_possible = saw_stop_possible || stop_token.stop_possible();
    requests.push_back(request);

    if (stop_token.stop_requested()) {
      return { .error_code = agent::make_error_code(agent::llm_error_code::cancelled) };
    }

    if (calls == 1) {
      return {
        .tool_calls = {
          {
            .id = "call-1",
            .name = "echo_text",
            .arguments_json = R"({"text":"from tool"})",
          },
        },
      };
    }

    return { .content = "final answer" };
  }

  int calls { 0 };
  bool saw_stop_possible { false };
  std::vector<llm_request> requests;
};

class blocking_client final : public llm_client {
public:
  llm_response complete(const llm_request& request) override {
    return complete(request, {});
  }

  llm_response complete(const llm_request&, std::stop_token stop_token) override {
    ++calls;
    while (!stop_token.stop_requested()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return { .error_code = agent::make_error_code(agent::llm_error_code::cancelled) };
  }

  int calls { 0 };
};

class streaming_capture_http_client final : public http_client {
public:
  explicit streaming_capture_http_client(std::vector<std::string> chunks)
      : chunks_(std::move(chunks)) {
  }

  http_response send(const http_request& request) override {
    requests.push_back(request);
    return response_;
  }

  http_response send_stream(
    const http_request& request,
    const http_stream_chunk_callback& on_chunk,
    std::stop_token stop_token = {}) override {
    requests.push_back(request);
    if (stop_token.stop_requested()) {
      return { .error_code = agent::make_error_code(agent::llm_error_code::cancelled) };
    }

    std::string body;
    for (const auto& chunk : chunks_) {
      body += chunk;
      if (on_chunk && !on_chunk(chunk)) {
        return { .error_code = std::make_error_code(std::errc::operation_canceled),
          .body = body };
      }
    }
    return { .body = body };
  }

  std::vector<http_request> requests;

private:
  std::vector<std::string> chunks_;
  http_response response_;
};

class stop_aware_provider {
public:
  std::vector<llm_tool> tools() const {
    return { make_llm_tool<echo_text>() };
  }

  llm_tool_result invoke(
    const std::string&,
    const std::string& arguments_json,
    std::stop_token stop_token) {
    saw_stop_possible = saw_stop_possible || stop_token.stop_possible();
    if (stop_token.stop_requested()) {
      return {
        .content = "cancelled",
        .error_code = agent::make_error_code(agent::llm_error_code::cancelled),
      };
    }
    return invoke_reflected_tool<echo_text>(arguments_json);
  }

  bool saw_stop_possible { false };
};

void test_public_tool_api_supports_names_schema_and_parse() {
  const auto tool = make_llm_tool<echo_text>();
  require(tool.name == "echo_text",
    "make_llm_tool should use the reflected tool type name: " + tool.name);
  require(tool.description == echo_text::description, "make_llm_tool should expose description");
  require(contains(tool.parameters_json_schema, R"("text")"),
    "make_llm_tool should expose reflected parameters");

  const auto parsed = parse_tool_arguments<echo_text>(R"({"text":"hello"})");
  require(parsed.text == "hello", "parse_tool_arguments should hydrate reflected arguments");

  tool_provider<echo_text> provider;
  const auto result = provider.invoke("echo_text", R"({"text":"provider"})");
  require(!result.error_code && result.content == "provider",
    "tool_provider should invoke tools by stable reflected name");
}

void test_public_tool_api_supports_stateful_context_tools() {
  const session_context context { .prefix = "session:" };
  const auto tool = make_llm_tool<session_lookup>();
  require(tool.name == "session_lookup", "stateful reflected tools should use reflected names");

  const auto result = invoke_reflected_tool<session_lookup>(R"({"key":"active-tab"})", context);
  require(!result.error_code, "context tool invocation should succeed: " + result.content);
  require(result.content == "session:active-tab",
    "context tool invocation should pass provider-owned state");
}

void test_runner_callbacks_and_stop_token_reach_provider() {
  tool_call_client client;
  auto provider = std::make_shared<stop_aware_provider>();
  llm_agent_runner runner(client, provider);
  std::stop_source stop_source;

  std::vector<std::string> events;
  llm_agent_run_options options;
  options.stop_token = stop_source.get_token();
  options.callbacks.on_tool_start = [&](const llm_tool_call& call) {
    events.push_back("tool_start:" + call.name);
  };
  options.callbacks.on_tool_result = [&](const llm_tool_call& call, const llm_tool_result& result) {
    events.push_back("tool_result:" + call.name + ":" + result.content);
  };
  options.callbacks.on_delta = [&](std::string_view delta) {
    events.push_back("delta:" + std::string(delta));
  };
  options.callbacks.on_done = [&](const llm_response& response) {
    events.push_back("done:" + response.content);
  };

  const auto response = runner.complete("use the tool", std::move(options));
  require(!response.error_code && response.content == "final answer",
    "runner should complete after tool round");
  require(client.calls == 2, "runner should call the model again after a tool result");
  require(provider->saw_stop_possible, "runner should pass stop_token to stop-aware providers");
  require(events.size() == 4, "runner should emit expected callback count");
  require(events[0] == "tool_start:echo_text", "runner should emit tool start");
  require(events[1] == "tool_result:echo_text:from tool", "runner should emit tool result");
  require(events[2] == "delta:final answer", "runner should emit final content delta");
  require(events[3] == "done:final answer", "runner should emit done");
}

void test_runner_pre_cancelled_request_does_not_call_model() {
  tool_call_client client;
  llm_agent_runner runner(client);
  std::stop_source stop_source;
  stop_source.request_stop();

  bool cancelled = false;
  llm_agent_run_options options;
  options.stop_token = stop_source.get_token();
  options.callbacks.on_cancelled = [&] {
    cancelled = true;
  };

  const auto response = runner.complete("should cancel", std::move(options));
  require(response.error_code == agent::llm_error_code::cancelled,
    "pre-cancelled runner request should return cancelled");
  require(cancelled, "pre-cancelled runner request should emit cancelled callback");
  require(client.calls == 0, "pre-cancelled runner request should not call model");
}

void test_async_runner_can_be_cancelled_by_handle() {
  blocking_client client;
  llm_agent_runner runner(client);
  std::stop_source external_stop_source;

  llm_agent_run_options options;
  options.stop_token = external_stop_source.get_token();

  auto run = runner.run_async("long request", std::move(options));
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  run.request_stop();

  const auto response = run.get();
  require(response.error_code == agent::llm_error_code::cancelled,
    "async runner handle cancellation should return cancelled even with an external token");
  require(!external_stop_source.stop_requested(),
    "async runner handle cancellation should not mutate the caller-owned stop source");
  require(client.calls == 1, "async runner should call model once");
}

void test_sse_parser_handles_split_and_batched_events() {
  sse_event_parser parser;
  std::vector<sse_event> events;
  const auto collect = [&](const sse_event& event) {
    events.push_back(event);
    return true;
  };

  require(parser.feed("event: token\ndata: Hel", collect), "first split SSE feed should pass");
  require(parser.feed("lo\nid: 1\n\ndata: [DONE]\n\n", collect),
    "second split SSE feed should pass");
  require(parser.finish(collect), "SSE finish should pass");

  require(events.size() == 2, "SSE parser should emit two events");
  require(events[0].event == "token", "SSE parser should retain event name");
  require(events[0].data == "Hello", "SSE parser should join split data line");
  require(events[0].id == "1", "SSE parser should retain event id");
  require(events[1].data == "[DONE]", "SSE parser should parse batched done event");
}

void test_openrouter_streaming_content_and_tool_call_accumulation() {
  auto http = std::make_shared<streaming_capture_http_client>(std::vector<std::string> {
    "data: {\"choices\":[{\"delta\":{\"content\":\"Hel\"},\"finish_reason\":null}]}\n\n"
    "data: {\"choices\":[{\"delta\":{\"content\":\"lo\"},\"finish_reason\":\"stop\"}],"
    "\"usage\":{\"prompt_tokens\":3,\"completion_tokens\":2,\"total_tokens\":5}}\n\n",
    "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_1\","
    "\"type\":\"function\",\"function\":{\"name\":\"get_\",\"arguments\":\"{\\\"city\\\"\"}}]},"
    "\"finish_reason\":null}]}\n\n",
    "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{"
    "\"name\":\"weather\",\"arguments\":\":\\\"Tokyo\\\"}\"}}]},"
    "\"finish_reason\":\"tool_calls\"}]}\n\n",
    "data: [DONE]\n\n",
  });

  openrouter_llm_client client({
    .base_url = "https://example.test",
    .api_key = "",
    .require_api_key = false,
    .model = "test-model",
    .max_retries = 0,
  }, http);

  llm_request request;
  request.messages.push_back({ .role = "user", .content = "hello" });

  std::vector<llm_stream_event> events;
  llm_stream_callbacks callbacks;
  callbacks.on_event = [&](const llm_stream_event& event) {
    events.push_back(event);
  };

  const auto response = client.complete_stream(request, callbacks);
  require(!response.error_code, "streaming response should succeed: " + response.error_code.message());
  require(response.content == "Hello", "streaming content deltas should aggregate");
  require(response.finish_reason == "tool_calls", "latest finish reason should be retained");
  require(response.usage.total_tokens == 5, "streaming usage should be parsed");
  require(response.tool_calls.size() == 1, "streaming tool call should be completed");
  require(response.tool_calls[0].id == "call_1", "streaming tool call id should be retained");
  require(response.tool_calls[0].name == "get_weather",
    "streaming tool call name deltas should aggregate");
  require(response.tool_calls[0].arguments_json == R"({"city":"Tokyo"})",
    "streaming tool call argument deltas should aggregate");

  int content_deltas = 0;
  int tool_deltas = 0;
  int tool_done = 0;
  int done = 0;
  for (const auto& event : events) {
    if (event.type == llm_stream_event_type::content_delta) {
      ++content_deltas;
    }
    if (event.type == llm_stream_event_type::tool_call_delta) {
      ++tool_deltas;
    }
    if (event.type == llm_stream_event_type::tool_call_done) {
      ++tool_done;
    }
    if (event.type == llm_stream_event_type::done) {
      ++done;
    }
  }
  require(content_deltas == 2, "streaming should emit both content deltas");
  require(tool_deltas == 2, "streaming should emit both tool call deltas");
  require(tool_done == 1, "streaming should emit completed tool call");
  require(done == 1, "streaming should emit done once");

  require(http->requests.size() == 1, "streaming should issue one HTTP request");
  const auto payload = nlohmann::json::parse(http->requests[0].body);
  require(payload.value("stream", false), "streaming request should set stream=true");
}

void test_openrouter_client_reports_missing_api_key_without_network() {
  openrouter_llm_client client({
    .base_url = "https://openrouter.ai/api",
    .api_key = "",
    .require_api_key = true,
    .model = "test-model",
    .max_retries = 0,
  });

  llm_request request;
  request.messages.push_back({ .role = "user", .content = "hello" });

  const auto response = client.complete(request);
  require(response.error_code == agent::llm_error_code::missing_api_key,
    "OpenRouter client should report missing API key before network I/O");
}

void run(const char* name, void (*test)()) {
  test();
  println("[PASS] {}", name);
}

} // namespace

int main() {
  try {
    run("public tool API supports names schema and parse",
      test_public_tool_api_supports_names_schema_and_parse);
    run("public tool API supports stateful context tools",
      test_public_tool_api_supports_stateful_context_tools);
    run("runner callbacks and stop token reach provider",
      test_runner_callbacks_and_stop_token_reach_provider);
    run("runner pre-cancelled request does not call model",
      test_runner_pre_cancelled_request_does_not_call_model);
    run("async runner can be cancelled by handle", test_async_runner_can_be_cancelled_by_handle);
    run("SSE parser handles split and batched events",
      test_sse_parser_handles_split_and_batched_events);
    run("OpenRouter streaming content and tool call accumulation",
      test_openrouter_streaming_content_and_tool_call_accumulation);
    run("openrouter client reports missing api key without network",
      test_openrouter_client_reports_missing_api_key_without_network);
  }
  catch (const std::exception& ex) {
    println("[FAIL] {}", ex.what());
    return 1;
  }

  return 0;
}
