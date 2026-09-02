#include <algorithm>
#include <cctype>
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
#include <wuwe/agent/llm/openai_compatible_llm_client.h>
#include <wuwe/agent/llm/openrouter_llm_client.h>
#include <wuwe/agent/tools/tool.hpp>
#include <wuwe/common/print.h>
#include <wuwe/common/sha256.hpp>
#include <wuwe/net/http_client.h>
#include <wuwe/net/http_status_code.h>
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

struct shout_text {
  static constexpr std::string_view description = "Uppercase text.";

  std::string text;

  std::string invoke() const {
    std::string result = text;
    for (auto& ch : result) {
      ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    return result;
  }
};

inline std::string large_output_payload() {
  return "HEAD-" + std::string(5'000, 'x') + "-TAIL";
}

struct large_output {
  static constexpr std::string_view description = "Return a deliberately large result.";

  std::string invoke() const {
    return large_output_payload();
  }
};

struct invalid_large_output {
  static constexpr std::string_view description = "Return a contract-invalid large result.";

  std::string invoke() const {
    return std::string(5'000, 'x');
  }
};

} // namespace agent_runtime_test_tools

namespace wuwe {

template<>
struct tool_contract<agent_runtime_test_tools::large_output> {
  static agent::tools::tool_descriptor descriptor() {
    auto value = agent::tools::descriptor_from_llm_tool(
      make_llm_tool<agent_runtime_test_tools::large_output>());
    value.model_output_projection = { .max_bytes = 256, .max_tokens = 1'000 };
    return value;
  }
};

template<>
struct tool_contract<agent_runtime_test_tools::invalid_large_output> {
  static agent::tools::tool_descriptor descriptor() {
    auto value = agent::tools::descriptor_from_llm_tool(
      make_llm_tool<agent_runtime_test_tools::invalid_large_output>());
    value.output_schema = {
      { "type", "object" },
      { "required", { "result" } },
      { "properties", { { "result", { { "type", "string" } } } } },
    };
    value.model_output_projection = { .max_bytes = 256, .max_tokens = 1'000 };
    return value;
  }
};

} // namespace wuwe

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

class promised_tool_client final : public llm_client {
public:
  llm_response complete(const llm_request& request) override {
    requests.push_back(request);
    if (requests.size() == 1) {
      return { .content = "I will call the tool now." };
    }
    if (requests.size() == 2) {
      return {
        .tool_calls = {
          { .id = "continued-call", .name = "echo_text",
            .arguments_json = R"({"text":"continued"})" },
        },
      };
    }
    return { .content = "terminal answer" };
  }

  std::vector<llm_request> requests;
};
class named_tool_call_client final : public llm_client {
public:
  explicit named_tool_call_client(std::string tool_name) : tool_name_(std::move(tool_name)) {
  }

  llm_response complete(const llm_request& request) override {
    requests.push_back(request);
    if (requests.size() == 1) {
      return {
        .tool_calls = { {
          .id = "projection-call",
          .name = tool_name_,
          .arguments_json = "{}",
        } },
      };
    }
    return { .content = "projection observed" };
  }

  std::vector<llm_request> requests;

private:
  std::string tool_name_;
};

class result_selective_throwing_estimator final : public agent::llm::context_token_estimator {
public:
  std::size_t estimate_text(std::string_view text) const override {
    if (text.find("from tool") != std::string_view::npos) {
      throw std::runtime_error("tool-result tokenizer unavailable");
    }
    return fallback_.estimate_text(text);
  }

  std::string truncate_text(
    std::string_view text, std::size_t token_limit, bool keep_tail) const override {
    return fallback_.truncate_text(text, token_limit, keep_tail);
  }

private:
  agent::llm::heuristic_context_token_estimator fallback_;
};

class byte_count_context_estimator final : public agent::llm::context_token_estimator {
public:
  std::size_t estimate_text(std::string_view text) const override {
    return text.size();
  }

  std::string truncate_text(
    std::string_view text, std::size_t token_limit, bool keep_tail) const override {
    if (text.size() <= token_limit) {
      return std::string(text);
    }
    return keep_tail ? std::string(text.substr(text.size() - token_limit))
                     : std::string(text.substr(0, token_limit));
  }
};

class model_callback_client final : public llm_client {
public:
  bool supports_streaming() const noexcept override {
    return true;
  }

  llm_response complete(const llm_request& request) override {
    ++completion_calls;
    requests.push_back(request);
    return {
      .content = "prepared response",
      .usage = { .prompt_tokens = 7, .completion_tokens = 3, .total_tokens = 10 },
    };
  }

  llm_response complete_stream(
    const llm_request& request, const llm_stream_callbacks&, std::stop_token = {}) override {
    ++streaming_calls;
    requests.push_back(request);
    return { .content = "unexpected streaming response" };
  }

  int completion_calls { 0 };
  int streaming_calls { 0 };
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

class endless_tool_call_client final : public llm_client {
public:
  llm_response complete(const llm_request& request) override {
    return complete(request, {});
  }

  llm_response complete(const llm_request& request, std::stop_token) override {
    ++calls;
    requests.push_back(request);
    return {
      .content = "still needs a tool",
      .tool_calls = {
        {
          .id = "call-" + std::to_string(calls),
          .name = "echo_text",
          .arguments_json = R"({"text":"again"})",
        },
      },
    };
  }

  int calls { 0 };
  std::vector<llm_request> requests;
};

class streaming_capture_http_client final : public http_client {
public:
  explicit streaming_capture_http_client(
    std::vector<std::string> chunks, http_response response = {})
      : chunks_(std::move(chunks)), response_(std::move(response)) {
  }

  http_response send(const http_request& request) override {
    requests.push_back(request);
    return response_;
  }

  http_response send_stream(const http_request& request, const http_stream_chunk_callback& on_chunk,
    std::stop_token stop_token = {}) override {
    requests.push_back(request);
    if (stop_token.stop_requested()) {
      return { .error_code = agent::make_error_code(agent::llm_error_code::cancelled) };
    }

    std::string body;
    for (const auto& chunk : chunks_) {
      body += chunk;
      if (on_chunk && !on_chunk(chunk)) {
        return { .error_code = std::make_error_code(std::errc::operation_canceled), .body = body };
      }
    }
    return { .body = body };
  }

  std::vector<http_request> requests;

private:
  std::vector<std::string> chunks_;
  http_response response_;
};

class delayed_streaming_http_client final : public http_client {
public:
  explicit delayed_streaming_http_client(std::vector<std::pair<int, std::string>> chunks)
      : chunks_(std::move(chunks)) {
  }

  http_response send(const http_request& request) override {
    requests.push_back(request);
    return {};
  }

  http_response send_stream(const http_request& request, const http_stream_chunk_callback& on_chunk,
    std::stop_token stop_token = {}) override {
    requests.push_back(request);
    std::string body;
    for (const auto& [delay_ms, chunk] : chunks_) {
      if (delay_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
      }
      if (stop_token.stop_requested()) {
        return { .error_code = agent::make_error_code(agent::llm_error_code::cancelled),
          .body = body };
      }
      body += chunk;
      if (on_chunk && !on_chunk(chunk)) {
        return { .error_code = std::make_error_code(std::errc::operation_canceled), .body = body };
      }
    }
    return { .body = body };
  }

  std::vector<http_request> requests;

private:
  std::vector<std::pair<int, std::string>> chunks_;
};

class streaming_error_after_chunks_http_client final : public http_client {
public:
  explicit streaming_error_after_chunks_http_client(std::vector<std::string> chunks)
      : chunks_(std::move(chunks)) {
  }

  http_response send(const http_request& request) override {
    requests.push_back(request);
    return {};
  }

  http_response send_stream(const http_request& request, const http_stream_chunk_callback& on_chunk,
    std::stop_token stop_token = {}) override {
    requests.push_back(request);
    if (stop_token.stop_requested()) {
      return { .error_code = agent::make_error_code(agent::llm_error_code::cancelled) };
    }

    std::string body;
    for (const auto& chunk : chunks_) {
      body += chunk;
      if (on_chunk && !on_chunk(chunk)) {
        return { .error_code = std::make_error_code(std::errc::operation_canceled), .body = body };
      }
    }

    return { .error_code = std::make_error_code(std::errc::connection_reset), .body = body };
  }

  std::vector<http_request> requests;

private:
  std::vector<std::string> chunks_;
};

class stop_aware_provider {
public:
  std::vector<llm_tool> tools() const {
    return { make_llm_tool<echo_text>() };
  }

  llm_tool_result invoke(
    const std::string&, const std::string& arguments_json, std::stop_token stop_token) {
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

bool has_request_header(const http_request& request, std::string_view name) {
  for (const auto& [key, value] : request.headers) {
    if (http_header_name_equals(key, name) && !value.empty()) {
      return true;
    }
  }
  return false;
}

class duplicate_echo_provider {
public:
  std::vector<llm_tool> tools() const {
    return { make_llm_tool<echo_text>() };
  }

  llm_tool_result invoke(const std::string&, const std::string&) const {
    return { .content = "duplicate" };
  }
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

void test_composite_tool_provider_routes_tools_in_order() {
  auto first = std::make_shared<tool_provider<echo_text>>();
  auto second = std::make_shared<tool_provider<shout_text>>();
  auto duplicate = std::make_shared<duplicate_echo_provider>();
  auto provider = compose_tool_providers(first, second, duplicate);

  const auto tools = provider->tools();
  require(tools.size() == 2, "composite provider should de-duplicate tool names");
  require(tools[0].name == "echo_text", "composite provider should preserve provider order");
  require(tools[1].name == "shout_text", "composite provider should include later provider tools");

  const auto echo = provider->invoke("echo_text", R"({"text":"first"})");
  require(!echo.error_code && echo.content == "first",
    "composite provider should route duplicate tool names to the first provider");

  const auto shout = provider->invoke("shout_text", R"({"text":"mixed"})");
  require(!shout.error_code && shout.content == "MIXED",
    "composite provider should route to later providers");

  const auto missing = provider->invoke("missing", "{}");
  require(missing.error_code == std::errc::function_not_supported,
    "composite provider should report missing tools");
  require(contains(missing.content, "echo_text") && contains(missing.content, "shout_text"),
    "composite provider missing-tool error should list available tools");
}

void test_composite_tool_provider_preserves_stop_token() {
  auto stop_aware = std::make_shared<stop_aware_provider>();
  auto provider = compose_tool_providers(stop_aware);
  std::stop_source stop_source;

  const auto result =
    provider->invoke("echo_text", R"({"text":"with stop"})", stop_source.get_token());

  require(!result.error_code && result.content == "with stop",
    "composite provider should invoke stop-aware providers");
  require(stop_aware->saw_stop_possible,
    "composite provider should preserve stop_token for child providers");
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

void test_runner_prepares_model_requests_and_observes_results() {
  model_callback_client client;
  llm_agent_runner runner(client);

  int prepare_calls = 0;
  int result_calls = 0;
  llm_request observed_request;
  llm_response observed_response;
  llm_agent_run_options options;
  options.callbacks.prepare_model_request = [&](llm_request request) {
    ++prepare_calls;
    request.model = "routed-model";
    request.max_output_tokens = 64;
    return std::optional<llm_request>(std::move(request));
  };
  options.callbacks.on_model_result = [&](
                                        const llm_request& request, const llm_response& response) {
    ++result_calls;
    observed_request = request;
    observed_response = response;
  };

  llm_request request;
  request.model = "original-model";
  request.messages.push_back({ .role = "user", .content = "prepare this request" });
  const auto response = runner.complete(std::move(request), std::move(options));

  require(!response.error_code && response.content == "prepared response",
    "prepared model request should complete normally");
  require(prepare_calls == 1 && result_calls == 1,
    "model preparation and result callbacks should run once per provider call");
  require(client.completion_calls == 1 && client.streaming_calls == 0,
    "model lifecycle callbacks alone should not force streaming");
  require(client.requests.size() == 1 && client.requests.front().model == "routed-model" &&
            client.requests.front().max_output_tokens == 64,
    "the provider should receive the prepared model request");
  require(observed_request.model == "routed-model" && observed_request.max_output_tokens == 64,
    "the result observer should receive the exact prepared request");
  require(
    observed_response.content == "prepared response" && observed_response.usage.total_tokens == 10,
    "the result observer should receive the completed provider response");
}

void test_runner_continues_after_nonterminal_model_response() {
  promised_tool_client client;
  auto provider = std::make_shared<duplicate_echo_provider>();
  llm_agent_run_options options;
  options.max_model_continuations = 1;
  options.callbacks.continue_after_model_response = [](
      const llm_request&, const llm_response& response, std::size_t used) {
    if (used == 0 && response.content == "I will call the tool now.") {
      return std::optional<llm_agent_model_continuation> {
        llm_agent_model_continuation {
          .instruction = "Call the promised tool now; do not narrate the action.",
          .tool_choice = llm_tool_choice {
            .mode = llm_tool_choice_mode::required,
          },
        }
      };
    }
    return std::optional<llm_agent_model_continuation> {};
  };

  const auto response = llm_agent_runner(client, provider).complete(
    "perform the task", std::move(options));
  require(!response.error_code && response.content == "terminal answer",
    "runner should continue a nonterminal prose response and execute the next tool call");
  require(client.requests.size() == 3,
    "runner should perform the forced continuation, tool round, and terminal model round");
  require(client.requests[1].tool_choice &&
      client.requests[1].tool_choice->mode == llm_tool_choice_mode::required,
    "continuation should apply its one-round tool choice contract");
  require(client.requests[1].messages.size() >= 3 &&
      client.requests[1].messages[1].role == "assistant" &&
      client.requests[1].messages[1].content == "I will call the tool now." &&
      client.requests[1].messages[2].role == "user",
    "continuation should preserve the nonterminal response and append the host instruction");
  require(!client.requests[2].tool_choice,
    "the continuation tool choice must not leak into later model rounds");
}

void test_runner_can_defer_assistant_memory_persistence() {
  model_callback_client client;
  agent::memory::memory_context memory;
  memory.set_scope({ .conversation_id = "deferred-memory" });
  llm_agent_runner runner(client, &memory);
  llm_agent_run_options options;
  options.persist_assistant_messages = false;

  const auto response = runner.complete("defer persistence", std::move(options));
  require(!response.error_code, "deferred assistant persistence should not affect completion");
  const auto records = memory.list();
  require(std::none_of(records.begin(),
            records.end(),
            [](const auto& record) {
              return record.metadata.contains("message_role") &&
                     record.metadata.at("message_role") == "assistant";
            }),
    "deferred persistence should keep raw assistant responses out of memory");
}

void test_runner_can_isolate_all_memory_persistence() {
  tool_call_client client;
  auto provider = std::make_shared<tool_provider<echo_text>>();
  agent::memory::memory_context memory;
  memory.set_scope({ .conversation_id = "isolated-memory" });
  llm_agent_runner runner(client, provider, &memory, 1);
  llm_agent_run_options options;
  options.persist_request_messages = false;
  options.persist_assistant_messages = false;
  options.persist_tool_messages = false;

  const auto response = runner.complete("do not persist this run", std::move(options));
  require(!response.error_code && response.content == "final answer",
    "memory isolation should not alter tool-loop execution");
  require(memory.list().empty(),
    "request, assistant, and tool persistence can be disabled as one isolation boundary");
}

void test_runner_projects_tool_output_only_at_the_model_boundary() {
  named_tool_call_client client("large_output");
  auto provider = std::make_shared<tool_provider<large_output>>();
  agent::memory::memory_context memory;
  memory.set_scope({ .conversation_id = "projected-tool-output" });
  llm_agent_runner runner(client, provider, &memory);
  auto store = std::make_shared<agent::runtime::in_memory_agent_run_store>();
  auto durable = std::make_shared<agent::runtime::agent_run_runtime>(store);

  std::string raw_callback_content;
  agent::llm::tool_output_projection_report projection_report;
  int projection_callbacks = 0;
  int truncation_events = 0;
  llm_agent_run_options options;
  options.context.run_id = "projected-tool-output-run";
  options.runtime = durable;
  options.tool_output_projection =
    agent::llm::tool_output_projection_policy { .max_bytes = 1'024, .max_tokens = 1'000 };
  options.callbacks.on_tool_result = [&](const llm_tool_call&, const llm_tool_result& result) {
    raw_callback_content = result.content;
  };
  options.callbacks.on_tool_output_projection =
    [&](const llm_tool_call&, const agent::llm::tool_output_projection_report& report) {
      ++projection_callbacks;
      projection_report = report;
    };
  options.callbacks.on_event = [&](const llm_agent_event& event) {
    if (event.type == llm_agent_event_type::tool_output_truncated) {
      ++truncation_events;
      require(event.tool_output_projection && event.tool_output_projection->truncated,
        "truncation events must carry their projection report during the callback");
    }
  };

  const auto response = runner.complete("return the large output", std::move(options));
  require(!response.error_code && response.content == "projection observed",
    "tool projection must not disturb the normal agent loop");
  require(raw_callback_content == large_output_payload(),
    "raw tool-result callbacks must retain the full unprojected outcome");
  require(projection_callbacks == 1 && truncation_events == 1 && projection_report.truncated &&
            projection_report.max_bytes == 256,
    "the tool descriptor must tighten the runner ceiling and emit one observable truncation");
  require(client.requests.size() == 2,
    "the model must receive a follow-up request after the projected result");
  const auto tool_message = std::find_if(client.requests[1].messages.begin(),
    client.requests[1].messages.end(),
    [](const chat_message& message) { return message.role == "tool"; });
  require(tool_message != client.requests[1].messages.end() &&
            tool_message->content.size() <= 256 &&
            tool_message->content.find("Warning: tool output truncated") != std::string::npos &&
            tool_message->content != raw_callback_content,
    "only the bounded projection may cross the model-facing Tool-message boundary");

  const auto records = memory.list();
  const auto memory_tool_result =
    std::find_if(records.begin(), records.end(), [](const auto& record) {
      const auto role = record.metadata.find("message_role");
      return role != record.metadata.end() && role->second == "tool";
    });
  require(
    memory_tool_result != records.end() && memory_tool_result->content == tool_message->content,
    "Memory must observe the exact projected Tool message");
  const auto durable_record = durable->get("projected-tool-output-run");
  require(
    durable_record &&
      durable_record->admitted_tool_results.at("projection-call").outcome.content ==
        large_output_payload() &&
      durable_record->tool_output_projections.at("projection-call").projected_content_sha256 ==
        common::sha256_hex(tool_message->content) &&
      durable_record->tool_output_projections.at("projection-call").report.max_bytes == 256,
    "durable state must link the complete raw outcome to a digest-only model projection audit");
}

void test_runner_reports_unchanged_projection_without_a_truncation_event() {
  tool_call_client client;
  auto provider = std::make_shared<tool_provider<echo_text>>();
  llm_agent_runner runner(client, provider);

  int projection_callbacks = 0;
  int truncation_events = 0;
  llm_agent_run_options options;
  options.callbacks.on_tool_output_projection =
    [&](const llm_tool_call&, const agent::llm::tool_output_projection_report& report) {
      ++projection_callbacks;
      require(!report.truncated &&
                report.limiting_factor == agent::llm::tool_output_projection_limit::none,
        "unchanged outputs must be reported as unchanged");
    };
  options.callbacks.on_event = [&](const llm_agent_event& event) {
    if (event.type == llm_agent_event_type::tool_output_truncated) {
      ++truncation_events;
    }
  };

  const auto response = runner.complete("use the small tool", std::move(options));
  require(!response.error_code && projection_callbacks == 1 && truncation_events == 0,
    "every tool result should be observable, but unchanged output must not emit truncation events");
}

void test_output_schema_validation_precedes_projection() {
  named_tool_call_client client("invalid_large_output");
  auto provider = std::make_shared<tool_provider<invalid_large_output>>();
  llm_agent_runner runner(client, provider);

  llm_tool_result observed_raw_result;
  llm_agent_run_options options;
  options.callbacks.prepare_tool_result = [](const llm_tool_call&, llm_tool_result result) {
    result.data = "not-an-object";
    return std::optional<llm_tool_result>(std::move(result));
  };
  options.callbacks.on_tool_result = [&](const llm_tool_call&, const llm_tool_result& result) {
    observed_raw_result = result;
  };

  const auto response = runner.complete("return invalid output", std::move(options));
  require(!response.error_code && observed_raw_result.error_code == std::errc::protocol_error &&
            observed_raw_result.error_category == agent::tools::tool_error_category::internal &&
            observed_raw_result.content == "tool result does not satisfy its output schema",
    "strict output-schema validation must transform invalid raw outcomes before projection");
  const auto& follow_up = client.requests.at(1);
  const auto tool_message = std::find_if(follow_up.messages.begin(),
    follow_up.messages.end(),
    [](const chat_message& message) { return message.role == "tool"; });
  require(tool_message != follow_up.messages.end(),
    "the validated tool failure must still be returned to the model");
  const auto projected_error = nlohmann::json::parse(tool_message->content);
  require(!projected_error.at("ok").get<bool>() &&
            projected_error.at("error").at("category") == "internal",
    "projection must encode the post-validation error rather than the invalid success payload");
}

void test_projection_failure_after_tool_completion_is_a_stable_runner_error() {
  tool_call_client client;
  auto provider = std::make_shared<tool_provider<echo_text>>();
  llm_agent_runner runner(client, provider);
  auto store = std::make_shared<agent::runtime::in_memory_agent_run_store>();
  auto durable = std::make_shared<agent::runtime::agent_run_runtime>(store);

  std::string raw_result;
  std::error_code callback_error;
  llm_agent_run_options options;
  options.context.run_id = "projection-failure-run";
  options.runtime = durable;
  options.token_estimator = std::make_shared<result_selective_throwing_estimator>();
  options.callbacks.on_tool_result = [&](const llm_tool_call&, const llm_tool_result& result) {
    raw_result = result.content;
  };
  options.callbacks.on_error = [&](std::error_code error, std::string_view) {
    callback_error = error;
  };

  const auto response = runner.complete("use the tool", std::move(options));
  require(response.error_code == agent::llm_error_code::tool_output_projection_failed &&
            response.stop_reason == "tool_output_projection_failed" &&
            response.metadata.at("tool_output_projection_error") == "estimator_failure",
    "post-tool projection failures must become a stable runner response instead of an exception");
  require(raw_result == "from tool" && client.calls == 1,
    "the complete raw result must remain observable without sending an invalid follow-up request");
  const auto record = durable->get("projection-failure-run");
  require(record && record->admitted_tool_results.at("call-1").outcome.content == "from tool",
    "durable admission must retain the raw result before projection failure is finalized");
  require(record->tool_output_projections.empty(),
    "failed projections must not create a model-visible projection audit record");
  require(callback_error == agent::llm_error_code::tool_output_projection_failed,
    "projection failures must reach the normal runner error callback");
}

void test_projection_policy_is_preflighted_before_model_and_tool_execution() {
  tool_call_client client;
  auto provider = std::make_shared<tool_provider<echo_text>>();
  llm_agent_runner runner(client, provider);

  llm_agent_run_options options;
  options.token_estimator = std::make_shared<byte_count_context_estimator>();
  options.tool_output_projection =
    agent::llm::tool_output_projection_policy { .max_bytes = 256, .max_tokens = 64 };
  bool rejected = false;
  try {
    (void)runner.complete("do not start", std::move(options));
  }
  catch (const std::invalid_argument&) {
    rejected = true;
  }
  require(rejected && client.calls == 0,
    "projection policies incompatible with the estimator must fail before model or tool work");
}

void test_runner_reports_tool_round_budget_exhaustion_with_stable_error() {
  endless_tool_call_client client;
  auto provider = std::make_shared<tool_provider<echo_text>>();
  llm_agent_runner runner(client, provider, 1);

  std::error_code callback_error;
  std::string callback_message;
  llm_agent_run_options options;
  options.callbacks.on_error = [&](std::error_code error, std::string_view message) {
    callback_error = error;
    callback_message = std::string(message);
  };

  const auto response = runner.complete("keep using tools", std::move(options));
  require(response.error_code == agent::llm_error_code::agent_loop_budget_exceeded,
    "runner should report a stable loop budget error");
  require(callback_error == agent::llm_error_code::agent_loop_budget_exceeded,
    "runner error callback should receive the stable loop budget error");
  require(callback_message.find("tool round budget") != std::string::npos,
    "runner error callback should explain the exhausted tool round budget");
  require(response.stop_reason == "tool_round_budget_exceeded",
    "runner should expose a stable stop reason");
  require(response.metadata.at("used_tool_rounds") == "1", "runner should report used tool rounds");
  require(response.metadata.at("max_tool_rounds") == "1", "runner should report max tool rounds");
  require(
    response.metadata.at("last_tool_call") == "echo_text", "runner should report last tool call");
  require(response.metadata.at("last_model_response") == "still needs a tool",
    "runner should report last model response before replacing user-facing content");
  require(response.error_code.message().find("resource unavailable") == std::string::npos,
    "runner should not leak resource-unavailable wording");
}

void test_runner_pre_cancelled_request_does_not_call_model() {
  tool_call_client client;
  llm_agent_runner runner(client);
  std::stop_source stop_source;
  stop_source.request_stop();

  bool cancelled = false;
  llm_agent_run_options options;
  options.stop_token = stop_source.get_token();
  options.callbacks.on_cancelled = [&] { cancelled = true; };

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

void test_async_runner_owns_runner_state() {
  blocking_client client;
  auto run = llm_agent_runner(client).run_async("temporary runner");
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  run.request_stop();
  const auto response = run.get();
  require(response.error_code == agent::llm_error_code::cancelled && client.calls == 1,
    "async execution remains valid after the originating runner is destroyed");
}

void test_runner_rejects_negative_tool_round_limits() {
  model_callback_client client;
  bool rejected = false;
  try {
    (void)llm_agent_runner(client, -1);
  }
  catch (const std::invalid_argument&) {
    rejected = true;
  }
  require(rejected, "agent runner rejects negative tool-round limits at construction");
}

void test_sse_parser_handles_split_and_batched_events() {
  sse_event_parser parser;
  std::vector<sse_event> events;
  const auto collect = [&](const sse_event& event) {
    events.push_back(event);
    return true;
  };

  require(parser.feed("event: token\ndata: Hel", collect), "first split SSE feed should pass");
  require(
    parser.feed("lo\nid: 1\n\ndata: [DONE]\n\n", collect), "second split SSE feed should pass");
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

  openrouter_llm_client client(
    {
      .base_url = "https://example.test",
      .api_key = "",
      .require_api_key = false,
      .model = "test-model",
      .max_retries = 0,
    },
    http);

  llm_request request;
  request.messages.push_back({ .role = "user", .content = "hello" });

  std::vector<llm_stream_event> events;
  llm_stream_callbacks callbacks;
  callbacks.on_event = [&](const llm_stream_event& event) { events.push_back(event); };

  const auto response = client.complete_stream(request, callbacks);
  require(
    !response.error_code, "streaming response should succeed: " + response.error_code.message());
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

void test_openai_compatible_streaming_separates_reasoning_from_content() {
  auto http = std::make_shared<streaming_capture_http_client>(std::vector<std::string> {
    "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"Checking inputs. \"},"
    "\"finish_reason\":null}]}\n\n",
    "data: {\"choices\":[{\"delta\":{\"content\":\"### Final\\n\"},"
    "\"finish_reason\":null}]}\n\n",
    "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"Ready.\"},"
    "\"finish_reason\":null}]}\n\n",
    "data: {\"choices\":[{\"delta\":{\"content\":\"Answer\"},"
    "\"finish_reason\":\"stop\"}]}\n\n",
    "data: [DONE]\n\n",
  });

  openai_compatible_llm_client client(
    {
      .base_url = "https://compatible.example",
      .api_key = "",
      .require_api_key = false,
      .model = "test-model",
      .max_retries = 0,
    },
    http);

  llm_request request;
  request.language = {
    .response_language = "zh-CN",
    .reasoning_language = "zh-CN",
  };
  request.messages.push_back({ .role = "user", .content = "hello" });

  std::vector<llm_stream_event> events;
  std::string reasoning_callback;
  llm_stream_callbacks callbacks;
  callbacks.on_event = [&](const llm_stream_event& event) { events.push_back(event); };
  callbacks.on_reasoning_delta = [&](std::string_view delta) { reasoning_callback += delta; };

  const auto response = client.complete_stream(request, callbacks);
  require(!response.error_code, "reasoning stream should succeed");
  require(response.content == "### Final\nAnswer",
    "content deltas should remain final answer content only");
  require(response.reasoning_summary == "Checking inputs. Ready.",
    "reasoning deltas should aggregate separately");
  require(response.reasoning_metadata.count("requested_language") == 1,
    "reasoning metadata should include requested reasoning language");
  require(response.reasoning_metadata.at("requested_language") == "zh-CN",
    "reasoning metadata should carry requested reasoning language");
  require(response.reasoning_metadata.count("detected_language") == 1,
    "reasoning metadata should include detected reasoning language");
  require(response.reasoning_metadata.at("detected_language") == "en",
    "reasoning metadata should detect obvious English reasoning");
  require(response.reasoning_metadata.count("language_mismatch") == 1,
    "reasoning metadata should include language mismatch state");
  require(response.reasoning_metadata.at("language_mismatch") == "true",
    "reasoning metadata should mark language mismatch");
  require(reasoning_callback == response.reasoning_summary,
    "reasoning callback should receive only reasoning deltas");
  const auto payload = nlohmann::json::parse(http->requests.front().body);
  require(payload["messages"].front().value("role", std::string {}) == "system",
    "reasoning language contract should be sent as a leading system message");
  require(
    payload["messages"].front().value("content", std::string {}).find("zh-CN") != std::string::npos,
    "reasoning language contract should include requested language");

  int content_deltas = 0;
  int reasoning_deltas = 0;
  int reasoning_done = 0;
  for (const auto& event : events) {
    if (event.type == llm_stream_event_type::content_delta) {
      ++content_deltas;
      require(event.reasoning_delta.empty(), "content events should not carry reasoning deltas");
    }
    if (event.type == llm_stream_event_type::reasoning_delta) {
      ++reasoning_deltas;
      require(event.content_delta.empty(), "reasoning events should not carry content deltas");
      require(event.reasoning_metadata.count("requested_language") == 1,
        "reasoning delta metadata should include requested language");
      require(event.reasoning_metadata.at("requested_language") == "zh-CN",
        "reasoning delta metadata should carry requested language");
    }
    if (event.type == llm_stream_event_type::reasoning_done) {
      ++reasoning_done;
      require(event.reasoning_summary == response.reasoning_summary,
        "reasoning_done should carry the final reasoning summary");
    }
  }
  require(content_deltas == 2, "streaming should emit content deltas separately");
  require(reasoning_deltas == 2, "streaming should emit reasoning deltas separately");
  require(reasoning_done == 1, "streaming should emit one reasoning_done event");
}

void test_agent_runner_forwards_structured_stream_events() {
  auto http = std::make_shared<streaming_capture_http_client>(std::vector<std::string> {
    "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"inspect\"},"
    "\"finish_reason\":null}]}\n\n",
    "data: {\"choices\":[{\"delta\":{\"content\":\"Hel\"},\"finish_reason\":null}]}\n\n",
    "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_1\","
    "\"type\":\"function\",\"function\":{\"name\":\"echo_\",\"arguments\":\"{\\\"text\\\"\"}}]},"
    "\"finish_reason\":null}]}\n\n",
    "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{"
    "\"name\":\"text\",\"arguments\":\":\\\"hi\\\"}\"}}]},"
    "\"finish_reason\":\"tool_calls\"}]}\n\n",
    "data: [DONE]\n\n",
  });

  openrouter_llm_client client(
    {
      .base_url = "https://example.test",
      .api_key = "",
      .require_api_key = false,
      .model = "test-model",
      .max_retries = 0,
    },
    http);

  llm_agent_runner runner(client, 0);
  llm_request request;
  request.messages.push_back({ .role = "user", .content = "hello" });

  std::vector<llm_stream_event> events;
  std::string text;
  llm_agent_run_options options;
  options.callbacks.on_delta = [&](std::string_view delta) { text += delta; };
  options.callbacks.on_stream_event = [&](
                                        const llm_stream_event& event) { events.push_back(event); };

  const auto response = runner.complete(std::move(request), std::move(options));
  require(response.error_code == agent::llm_error_code::agent_loop_budget_exceeded,
    "agent runner with max_tool_rounds=0 should stop before executing streamed tool call");
  require(text == "Hel", "agent runner should preserve legacy content delta callback");

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
  require(content_deltas == 1, "agent runner should forward content stream events");
  require(tool_deltas == 2, "agent runner should forward tool call delta events");
  require(tool_done == 1, "agent runner should forward completed tool call events");
  require(done == 1, "agent runner should forward done stream events");
}

void test_agent_runner_stream_event_callback_enables_streaming_without_text_delta() {
  auto http = std::make_shared<streaming_capture_http_client>(std::vector<std::string> {
    "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_1\","
    "\"type\":\"function\",\"function\":{\"name\":\"echo_text\",\"arguments\":\"{}\"}}]},"
    "\"finish_reason\":\"tool_calls\"}]}\n\n",
    "data: [DONE]\n\n",
  });

  openrouter_llm_client client(
    {
      .base_url = "https://example.test",
      .api_key = "",
      .require_api_key = false,
      .model = "test-model",
      .max_retries = 0,
    },
    http);

  llm_agent_runner runner(client, 0);
  llm_request request;
  request.messages.push_back({ .role = "user", .content = "hello" });

  int tool_deltas = 0;
  llm_agent_run_options options;
  options.callbacks.on_stream_event = [&](const llm_stream_event& event) {
    if (event.type == llm_stream_event_type::tool_call_delta) {
      ++tool_deltas;
    }
  };

  (void)runner.complete(std::move(request), std::move(options));
  require(tool_deltas == 1,
    "agent runner should use streaming when only structured stream callback is registered");
  require(http->requests.size() == 1, "agent runner should issue one streaming request");
  const auto payload = nlohmann::json::parse(http->requests[0].body);
  require(payload.value("stream", false),
    "agent runner should set stream=true for structured-only streaming callbacks");
}

void test_agent_runner_emits_agent_native_streaming_events() {
  auto http = std::make_shared<streaming_capture_http_client>(std::vector<std::string> {
    "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"inspect\"},"
    "\"finish_reason\":null}]}\n\n",
    "data: {\"choices\":[{\"delta\":{\"content\":\"Hel\"},\"finish_reason\":null}]}\n\n",
    "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_1\","
    "\"type\":\"function\",\"function\":{\"name\":\"echo_\",\"arguments\":\"{\\\"text\\\"\"}}]},"
    "\"finish_reason\":null}]}\n\n",
    "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{"
    "\"name\":\"text\",\"arguments\":\":\\\"hi\\\"}\"}}]},"
    "\"finish_reason\":\"tool_calls\"}]}\n\n",
    "data: [DONE]\n\n",
  });

  openrouter_llm_client client(
    {
      .base_url = "https://example.test",
      .api_key = "",
      .require_api_key = false,
      .model = "test-model",
      .max_retries = 0,
    },
    http);

  llm_agent_runner runner(client, 0);
  llm_request request;
  request.messages.push_back({ .role = "user", .content = "hello" });

  std::vector<llm_agent_event_type> events;
  llm_agent_run_options options;
  options.callbacks.on_event = [&](const llm_agent_event& event) { events.push_back(event.type); };

  (void)runner.complete(std::move(request), std::move(options));

  const auto has = [&](llm_agent_event_type type) {
    return std::find(events.begin(), events.end(), type) != events.end();
  };
  require(
    has(llm_agent_event_type::model_started), "agent event stream should include model_started");
  require(has(llm_agent_event_type::model_first_event),
    "agent event stream should include model_first_event");
  require(has(llm_agent_event_type::model_content_delta),
    "agent event stream should include model_content_delta");
  require(has(llm_agent_event_type::model_reasoning_delta),
    "agent event stream should include model_reasoning_delta");
  require(has(llm_agent_event_type::model_reasoning_completed),
    "agent event stream should include model_reasoning_completed");
  require(has(llm_agent_event_type::tool_call_building),
    "agent event stream should include tool_call_building");
  require(has(llm_agent_event_type::tool_call_ready),
    "agent event stream should include tool_call_ready");
  require(has(llm_agent_event_type::model_completed),
    "agent event stream should include model_completed");
  require(has(llm_agent_event_type::agent_failed),
    "agent event stream should include agent_failed when tool round budget stops the run");
}

void test_openai_compatible_streaming_uses_stage_timeout_options() {
  auto http = std::make_shared<streaming_capture_http_client>(std::vector<std::string> {
    "data: {\"choices\":[{\"delta\":{\"content\":\"ok\"},\"finish_reason\":\"stop\"}]}\n\n",
    "data: [DONE]\n\n",
  });

  openrouter_llm_client client({
    .base_url = "https://example.test",
    .api_key = "",
    .require_api_key = false,
    .model = "test-model",
    .timeout = 30000,
    .stream_timeouts = {
      .total_ms = 9000,
      .connect_ms = 1000,
      .first_event_ms = 2000,
      .idle_ms = 3000,
    },
    .max_retries = 0,
  }, http);

  llm_request request;
  request.messages.push_back({ .role = "user", .content = "hello" });
  const auto response = client.complete_stream(request, {});

  require(!response.error_code, "streaming with stage timeouts should succeed");
  require(http->requests.size() == 1, "streaming timeout test should issue one request");
  require(http->requests[0].timeouts.total_ms == 9000,
    "streaming total timeout should map to HTTP total timeout");
  require(http->requests[0].timeouts.connect_ms == 1000,
    "streaming connect timeout should map to HTTP connect timeout");
  require(http->requests[0].timeouts.read_ms == 3000,
    "streaming idle timeout should map to HTTP read timeout");
}

void test_openai_compatible_streaming_reports_first_event_timeout() {
  auto http =
    std::make_shared<delayed_streaming_http_client>(std::vector<std::pair<int, std::string>> {
      {
        10,
        "data: {\"choices\":[{\"delta\":{\"content\":\"late\"},"
        "\"finish_reason\":\"stop\"}]}\n\n",
      },
    });

  openrouter_llm_client client({
    .base_url = "https://example.test",
    .api_key = "",
    .require_api_key = false,
    .model = "test-model",
    .stream_timeouts = {
      .first_event_ms = 1,
    },
    .max_retries = 0,
  }, http);

  llm_request request;
  request.messages.push_back({ .role = "user", .content = "hello" });
  std::vector<llm_stream_event> events;
  llm_stream_callbacks callbacks;
  callbacks.on_event = [&](const llm_stream_event& event) { events.push_back(event); };

  const auto response = client.complete_stream(request, callbacks);
  require(response.error_code == agent::llm_error_code::timeout,
    "late first streaming event should return stable timeout error");
  require(response.metadata.at("timeout_phase") == "first_event",
    "first event timeout should record timeout phase");
  require(response.metadata.at("timeout_ms") == "1",
    "first event timeout should record configured budget");
  require(!events.empty() && events.front().type == llm_stream_event_type::error,
    "first event timeout should emit stream error event");
}

void test_openai_compatible_streaming_reports_idle_timeout() {
  auto http =
    std::make_shared<delayed_streaming_http_client>(std::vector<std::pair<int, std::string>> {
      {
        0,
        "data: {\"choices\":[{\"delta\":{\"content\":\"first\"},"
        "\"finish_reason\":null}]}\n\n",
      },
      {
        10,
        "data: {\"choices\":[{\"delta\":{\"content\":\"late\"},"
        "\"finish_reason\":\"stop\"}]}\n\n",
      },
    });

  openrouter_llm_client client({
    .base_url = "https://example.test",
    .api_key = "",
    .require_api_key = false,
    .model = "test-model",
    .stream_timeouts = {
      .idle_ms = 1,
    },
    .max_retries = 0,
  }, http);

  llm_request request;
  request.messages.push_back({ .role = "user", .content = "hello" });
  std::vector<llm_stream_event> events;
  llm_stream_callbacks callbacks;
  callbacks.on_event = [&](const llm_stream_event& event) { events.push_back(event); };

  const auto response = client.complete_stream(request, callbacks);
  require(response.error_code == agent::llm_error_code::timeout,
    "idle streaming gap should return stable timeout error");
  require(
    response.metadata.at("timeout_phase") == "idle", "idle timeout should record timeout phase");
  require(
    response.metadata.at("timeout_ms") == "1", "idle timeout should record configured budget");
  require(!events.empty() && events.back().type == llm_stream_event_type::error,
    "idle timeout should emit stream error event");
}

void test_openai_compatible_streaming_ignores_tail_transport_error_after_output() {
  auto http = std::make_shared<streaming_error_after_chunks_http_client>(std::vector<std::string> {
    "data: {\"id\":\"chunk-1\",\"object\":\"chat.completion.chunk\","
    "\"choices\":[{\"delta\":{\"content\":\"DeepSeek answer\"},"
    "\"finish_reason\":null}]}\n\n",
    "data: {\"id\":\"chunk-2\",\"object\":\"chat.completion.chunk\","
    "\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n",
  });

  openai_compatible_llm_client client(
    {
      .base_url = "https://deepseek-compatible.example",
      .api_key = "",
      .require_api_key = false,
      .model = "deepseek-test-model",
      .max_retries = 0,
    },
    http);

  llm_request request;
  request.messages.push_back({ .role = "user", .content = "hello" });

  std::vector<llm_stream_event> events;
  llm_stream_callbacks callbacks;
  callbacks.on_event = [&](const llm_stream_event& event) { events.push_back(event); };

  const auto response = client.complete_stream(request, callbacks);
  require(!response.error_code,
    "streaming response with valid output should not fail on a tail transport error");
  require(response.content == "DeepSeek answer", "streaming response should retain parsed content");
  require(!contains(response.content, "data:"),
    "streaming response should not expose raw SSE frames as content");
  require(response.metadata.count("ignored_stream_transport_error") == 1,
    "streaming response should record ignored tail transport error metadata");

  int errors = 0;
  int done = 0;
  for (const auto& event : events) {
    if (event.type == llm_stream_event_type::error) {
      ++errors;
      require(!contains(event.message, "data:"),
        "streaming error callbacks should not expose raw SSE frames");
    }
    if (event.type == llm_stream_event_type::done) {
      ++done;
    }
  }
  require(errors == 0, "valid streamed output should suppress tail transport error callbacks");
  require(done == 1, "valid streamed output should emit a single done event");
}

void test_openai_compatible_streaming_invalid_event_uses_sanitized_error() {
  auto http = std::make_shared<streaming_capture_http_client>(std::vector<std::string> {
    "data: not-json\n\n",
  });

  openai_compatible_llm_client client(
    {
      .base_url = "https://compatible.example",
      .api_key = "",
      .require_api_key = false,
      .model = "test-model",
      .max_retries = 0,
    },
    http);

  llm_request request;
  request.messages.push_back({ .role = "user", .content = "hello" });

  std::vector<llm_stream_event> events;
  llm_stream_callbacks callbacks;
  callbacks.on_event = [&](const llm_stream_event& event) { events.push_back(event); };

  const auto response = client.complete_stream(request, callbacks);
  require(response.error_code == agent::llm_error_code::invalid_response,
    "invalid streaming event should fail with invalid_response");
  require(!contains(response.content, "data:") && !contains(response.content, "not-json"),
    "invalid streaming event should not expose raw SSE data");

  int errors = 0;
  for (const auto& event : events) {
    if (event.type == llm_stream_event_type::error) {
      ++errors;
      require(!contains(event.message, "data:") && !contains(event.message, "not-json"),
        "invalid streaming error callback should not expose raw SSE data");
    }
  }
  require(errors == 1, "invalid streaming event should emit one sanitized error");
}

void test_openai_compatible_and_openrouter_config_boundaries() {
  llm_request request;
  request.messages.push_back({ .role = "user", .content = "hello" });

  auto compatible_http =
    std::make_shared<streaming_capture_http_client>(std::vector<std::string> {});
  openai_compatible_llm_client compatible(
    {
      .base_url = "https://compatible.example",
      .api_key = "",
      .require_api_key = false,
      .model = "test-model",
      .max_retries = 0,
    },
    compatible_http);
  (void)compatible.complete(request);
  require(
    compatible_http->requests.size() == 1, "OpenAI-compatible client should issue one request");
  require(compatible_http->requests[0].url == "https://compatible.example/v1/chat/completions",
    "OpenAI-compatible client should use the configured base URL");
  require(!has_request_header(compatible_http->requests[0], "HTTP-Referer"),
    "OpenAI-compatible client should not add OpenRouter referer by default");
  require(!has_request_header(compatible_http->requests[0], "X-Title"),
    "OpenAI-compatible client should not add OpenRouter title by default");

  auto openrouter_http =
    std::make_shared<streaming_capture_http_client>(std::vector<std::string> {});
  openrouter_llm_client openrouter(
    {
      .api_key = "",
      .require_api_key = false,
      .model = "test-model",
      .max_retries = 0,
    },
    openrouter_http);
  (void)openrouter.complete(request);
  require(openrouter_http->requests.size() == 1, "OpenRouter preset should issue one request");
  require(openrouter_http->requests[0].url == "https://openrouter.ai/api/v1/chat/completions",
    "OpenRouter preset should provide the OpenRouter base URL");
  require(has_request_header(openrouter_http->requests[0], "HTTP-Referer"),
    "OpenRouter preset should add OpenRouter referer header by default");
  require(has_request_header(openrouter_http->requests[0], "X-Title"),
    "OpenRouter preset should add OpenRouter title header by default");

  auto openrouter_without_attribution_http =
    std::make_shared<streaming_capture_http_client>(std::vector<std::string> {});
  openrouter_llm_client openrouter_without_attribution(
    {
      .api_key = "",
      .require_api_key = false,
      .model = "test-model",
      .max_retries = 0,
      .referer_url = std::string {},
      .app_title = std::string {},
    },
    openrouter_without_attribution_http);
  (void)openrouter_without_attribution.complete(request);
  require(openrouter_without_attribution_http->requests.size() == 1,
    "OpenRouter preset with explicit empty attribution should issue one request");
  require(!has_request_header(openrouter_without_attribution_http->requests[0], "HTTP-Referer"),
    "OpenRouter preset should allow callers to suppress referer attribution");
  require(!has_request_header(openrouter_without_attribution_http->requests[0], "X-Title"),
    "OpenRouter preset should allow callers to suppress title attribution");
}

void test_openai_compatible_client_accepts_numeric_error_fields() {
  llm_request request;
  request.messages.push_back({ .role = "user", .content = "hello" });

  auto completion_http =
    std::make_shared<streaming_capture_http_client>(std::vector<std::string> {},
      http_response {
        .error_code = http_status_code::not_found,
        .status_code = 404,
        .body = R"({"error":{"message":"No endpoints found for this model","code":404}})",
      });

  openai_compatible_llm_client completion_client(
    {
      .base_url = "https://openrouter.ai/api/v1",
      .api_key = "",
      .require_api_key = false,
      .model = "test-model",
      .max_retries = 0,
    },
    completion_http);

  const auto completion_response = completion_client.complete(request);
  require(completion_response.error_code == agent::llm_error_code::model_unavailable,
    "numeric OpenAI-compatible error code 404 should classify as model_unavailable");
  require(completion_response.content == "No endpoints found for this model",
    "numeric OpenAI-compatible error fields should preserve the provider message");

  auto streaming_http = std::make_shared<streaming_capture_http_client>(std::vector<std::string> {
    "data: {\"error\":{\"message\":\"No endpoints found for this model\",\"code\":404}}\n\n",
  });

  openai_compatible_llm_client streaming_client(
    {
      .base_url = "https://openrouter.ai/api/v1",
      .api_key = "",
      .require_api_key = false,
      .model = "test-model",
      .max_retries = 0,
    },
    streaming_http);

  const auto streaming_response = streaming_client.complete_stream(request, {});
  require(streaming_response.error_code == agent::llm_error_code::model_unavailable,
    "numeric streaming OpenAI-compatible error code 404 should classify as model_unavailable");
  require(streaming_response.content == "No endpoints found for this model",
    "numeric streaming OpenAI-compatible error fields should preserve the provider message");
}

void test_openai_compatible_client_reports_missing_api_key_without_network() {
  openai_compatible_llm_client client({
    .base_url = "https://openrouter.ai/api",
    .api_key = "",
    .require_api_key = true,
    .load_api_key_from_environment = false,
    .model = "test-model",
    .max_retries = 0,
  });

  llm_request request;
  request.messages.push_back({ .role = "user", .content = "hello" });

  const auto response = client.complete(request);
  require(response.error_code == agent::llm_error_code::missing_api_key,
    "OpenAI-compatible client should report missing API key before network I/O");
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
    run("composite tool provider routes tools in order",
      test_composite_tool_provider_routes_tools_in_order);
    run("composite tool provider preserves stop token",
      test_composite_tool_provider_preserves_stop_token);
    run("runner callbacks and stop token reach provider",
      test_runner_callbacks_and_stop_token_reach_provider);
    run("runner prepares model requests and observes results",
      test_runner_prepares_model_requests_and_observes_results);
    run("runner continues after nonterminal model response",
      test_runner_continues_after_nonterminal_model_response);
    run("runner can defer assistant memory persistence",
      test_runner_can_defer_assistant_memory_persistence);
    run(
      "runner can isolate all memory persistence", test_runner_can_isolate_all_memory_persistence);
    run("runner projects tool output at model boundary",
      test_runner_projects_tool_output_only_at_the_model_boundary);
    run("runner observes unchanged tool projections",
      test_runner_reports_unchanged_projection_without_a_truncation_event);
    run("tool output validation precedes projection",
      test_output_schema_validation_precedes_projection);
    run("projection failure is a stable runner error",
      test_projection_failure_after_tool_completion_is_a_stable_runner_error);
    run("projection policy preflight precedes execution",
      test_projection_policy_is_preflighted_before_model_and_tool_execution);
    run("runner reports tool round budget exhaustion with stable error",
      test_runner_reports_tool_round_budget_exhaustion_with_stable_error);
    run("runner pre-cancelled request does not call model",
      test_runner_pre_cancelled_request_does_not_call_model);
    run("async runner can be cancelled by handle", test_async_runner_can_be_cancelled_by_handle);
    run("async runner owns runner state", test_async_runner_owns_runner_state);
    run("runner validates tool round limits", test_runner_rejects_negative_tool_round_limits);
    run("SSE parser handles split and batched events",
      test_sse_parser_handles_split_and_batched_events);
    run("OpenRouter streaming content and tool call accumulation",
      test_openrouter_streaming_content_and_tool_call_accumulation);
    run("OpenAI-compatible streaming separates reasoning from content",
      test_openai_compatible_streaming_separates_reasoning_from_content);
    run("agent runner forwards structured stream events",
      test_agent_runner_forwards_structured_stream_events);
    run("agent runner stream event callback enables streaming without text delta",
      test_agent_runner_stream_event_callback_enables_streaming_without_text_delta);
    run("agent runner emits agent native streaming events",
      test_agent_runner_emits_agent_native_streaming_events);
    run("OpenAI-compatible streaming uses stage timeout options",
      test_openai_compatible_streaming_uses_stage_timeout_options);
    run("OpenAI-compatible streaming reports first event timeout",
      test_openai_compatible_streaming_reports_first_event_timeout);
    run("OpenAI-compatible streaming reports idle timeout",
      test_openai_compatible_streaming_reports_idle_timeout);
    run("OpenAI-compatible streaming ignores tail transport error after output",
      test_openai_compatible_streaming_ignores_tail_transport_error_after_output);
    run("OpenAI-compatible streaming invalid event uses sanitized error",
      test_openai_compatible_streaming_invalid_event_uses_sanitized_error);
    run("OpenAI-compatible and OpenRouter config boundaries",
      test_openai_compatible_and_openrouter_config_boundaries);
    run("OpenAI-compatible client accepts numeric error fields",
      test_openai_compatible_client_accepts_numeric_error_fields);
    run("OpenAI-compatible client reports missing api key without network",
      test_openai_compatible_client_reports_missing_api_key_without_network);
  }
  catch (const std::exception& ex) {
    println("[FAIL] {}", ex.what());
    return 1;
  }

  return 0;
}
