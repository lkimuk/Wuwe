#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <wuwe/agent/llm/llm_agent_runner.h>
#include <wuwe/agent/llm/llm_error.h>
#include <wuwe/agent/llm/openrouter_llm_client.h>
#include <wuwe/agent/tools/tool.hpp>
#include <wuwe/common/print.h>

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
    run("openrouter client reports missing api key without network",
      test_openrouter_client_reports_missing_api_key_without_network);
  }
  catch (const std::exception& ex) {
    println("[FAIL] {}", ex.what());
    return 1;
  }

  return 0;
}
