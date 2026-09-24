#include <wuwe/agent/auth/oauth_credential_store.h>
#include <wuwe/agent/llm/codex_llm_client.h>
#include <wuwe/agent/llm/llm_agent_runner.h>
#include <wuwe/net/http_client.h>
#include <wuwe/net/sse_event_parser.h>
#include <wuwe/net/transport_error.h>

#include <condition_variable>
#include <deque>
#include <future>
#include <iostream>
#include <latch>
#include <mutex>
#include <stdexcept>

using namespace wuwe;
using namespace std::chrono_literals;
using json = nlohmann::json;
using agent::llm_error_code;
namespace {
void require(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}
const oauth_account_key key { "codex_oauth", "local-1" };
oauth_account_info info() {
  return { key, "Test", "workspace-1", oauth_account_state::ready, "subject-1" };
}
oauth_tokens tokens() {
  return { "test-access", "test-refresh", "test-id", std::chrono::system_clock::now() + 1h };
}
codex_llm_options options() {
  return { .model = "catalog-model", .client_version = "0.153.4" };
}
llm_tool tool() {
  return { "echo",
    "Echo text",
    R"({"type":"object","properties":{"text":{"type":"string"}},"required":["text"]})" };
}
llm_request request(bool tools = false) {
  llm_request r;
  r.messages.push_back({ .role = "system", .content = "Be concise." });
  r.messages.push_back({ .role = "user", .content = "Hello" });
  if (tools)
    r.tools.push_back(tool());
  return r;
}
std::string event(json data) {
  return "event: " + data.at("type").get<std::string>() + "\r\ndata: " + data.dump() + "\r\n\r\n";
}
json message(std::string content = "Hello!") {
  return {
    { "type", "message" },
    { "id", "msg-1" },
    { "role", "assistant" },
    { "status", "completed" },
    { "content",
      json::array(
        { { { "type", "output_text" }, { "text", content }, { "annotations", json::array() } } }) }
  };
}
json reasoning() {
  return { { "type", "reasoning" },
    { "id", "rs-1" },
    { "encrypted_content", "opaque-encrypted-state" },
    { "summary", json::array({ { { "type", "summary_text" }, { "text", "Plan." } } }) } };
}
json call(std::string id = "call-1", std::string arguments = R"({"text":"world"})") {
  return { { "type", "function_call" },
    { "id", "fc-" + id },
    { "call_id", id },
    { "name", "echo" },
    { "arguments", arguments },
    { "status", "completed" } };
}
json completed(json output) {
  json usage { { "input_tokens", 10 },
    { "output_tokens", 6 },
    { "total_tokens", 16 },
    { "input_tokens_details", { { "cached_tokens", 4 } } },
    { "output_tokens_details", { { "reasoning_tokens", 2 } } } };
  json response { { "id", "resp-1" },
    { "status", "completed" },
    { "output", std::move(output) },
    { "usage", std::move(usage) } };
  return { { "type", "response.completed" }, { "response", std::move(response) } };
}
std::string text_stream(std::string text = "Hello!") {
  return event({ { "type", "response.created" }, { "response", { { "id", "resp-1" } } } }) +
         event({ { "type", "response.output_text.delta" },
           { "item_id", "msg-1" },
           { "output_index", 0 },
           { "content_index", 0 },
           { "delta", text } }) +
         event({ { "type", "response.output_item.done" },
           { "output_index", 0 },
           { "item", message(text) } }) +
         event(completed(json::array({ message(text) })));
}
std::string tool_stream() {
  auto initial = call();
  initial["arguments"] = "";
  initial["status"] = "in_progress";
  return event({ { "type", "response.output_item.done" },
           { "output_index", 0 },
           { "item", reasoning() } }) +
         event({ { "type", "response.output_item.added" },
           { "output_index", 1 },
           { "item", initial } }) +
         event({ { "type", "response.function_call_arguments.delta" },
           { "item_id", "fc-call-1" },
           { "output_index", 1 },
           { "delta", "{\"text\":" } }) +
         event({ { "type", "response.function_call_arguments.delta" },
           { "item_id", "fc-call-1" },
           { "output_index", 1 },
           { "delta", "\"world\"}" } }) +
         event({ { "type", "response.function_call_arguments.done" },
           { "item_id", "fc-call-1" },
           { "output_index", 1 },
           { "arguments", R"({"text":"world"})" } }) +
         event(
           { { "type", "response.output_item.done" }, { "output_index", 1 }, { "item", call() } }) +
         event(completed(json::array({ reasoning(), call() })));
}
class scripted_http final : public http_client {
public:
  using action = std::function<http_response(
    const http_request&, const http_stream_chunk_callback&, std::stop_token)>;
  std::deque<action> steps;
  std::vector<http_request> requests;
  http_response send(const http_request&) override {
    throw std::runtime_error("stream only");
  }
  http_response send_stream(const http_request& req, const http_stream_chunk_callback& chunk,
    std::stop_token stop) override {
    require(!steps.empty(), "unexpected HTTP call");
    requests.push_back(req);
    auto fn = std::move(steps.front());
    steps.pop_front();
    return fn(req, chunk, stop);
  }
  void push(std::string stream, std::size_t split = 0, int status = 200) {
    steps.push_back(
      [stream = std::move(stream), split, status](const auto&, const auto& chunk, auto) {
        const auto size = split == 0 ? stream.size() : split;
        for (std::size_t i = 0; i < stream.size(); i += size) {
          if (!chunk(std::string_view(stream).substr(i, size)))
            return http_response { .error_code = transport_error::aborted_by_callback,
              .transport_error = transport_error::aborted_by_callback,
              .status_code = status };
        }
        return http_response { .status_code = status };
      });
  }
};
struct fixture {
  oauth_account_manager accounts { make_memory_oauth_credential_store(), {} };
  std::shared_ptr<scripted_http> http = std::make_shared<scripted_http>();
  fixture() {
    require(!accounts.save_authorization(info(), tokens()), "account setup failed");
  }
};

void test_request_and_text_stream() {
  fixture f;
  codex_llm_client client(f.accounts, key, options(), f.http);
  const std::string answer = "Hello \xe4\xb8\x96\xe7\x95\x8c!";
  f.http->push(": keepalive\r\n\r\n" + text_stream(answer), 1);
  auto input = request();
  input.language.response_language = "zh-CN";
  std::string content;
  int done = 0, errors = 0;
  auto result = client.complete_stream(input, { .on_event = [&](const auto& e) {
    content += e.content_delta;
    done += e.type == llm_stream_event_type::done;
    errors += e.type == llm_stream_event_type::error;
  } });
  require(
    !result.error_code && result.content == answer && content == answer && done == 1 && errors == 0,
    "split UTF-8 stream failed");
  require(!result.provider_state, "plain text unexpectedly retained provider state");
  require(result.usage.total_tokens == 16 && result.usage.cached_prompt_tokens == 4 &&
            result.usage.reasoning_tokens == 2,
    "usage mapping failed");
  const auto& wire = f.http->requests[0];
  const auto body = json::parse(wire.body);
  require(wire.url == "https://chatgpt.com/backend-api/codex/responses" && wire.method == "POST" &&
            !wire.follow_redirects && wire.tls.verify_peer && wire.tls.verify_host,
    "unsafe generation endpoint");
  auto has = [&](std::string name, std::string value) {
    return std::find(wire.headers.begin(), wire.headers.end(), std::pair(name, value)) !=
           wire.headers.end();
  };
  require(has("Authorization", "Bearer test-access") && has("chatgpt-account-id", "workspace-1") &&
            has("version", "0.153.4") && has("Accept", "text/event-stream"),
    "account headers lost");
  require(body["stream"] == true && body["store"] == false && body["tools"].is_array() &&
            body["input"][0]["type"] == "message" &&
            body["instructions"].get<std::string>().find("zh-CN") != std::string::npos &&
            body["include"][0] == "reasoning.encrypted_content" && !body.contains("temperature") &&
            !body.contains("max_output_tokens"),
    "wrong Codex contract");
  f.http->push(text_stream(answer));
  auto aggregate = client.complete(input);
  require(!aggregate.error_code && aggregate.content == result.content,
    "complete differs from streaming");
  f.http->push(event(completed(json::array({ message("snapshot only") }))));
  require(
    client.complete(input).content == "snapshot only", "completed-only snapshot not reconciled");
}
void test_tool_stream_and_state() {
  fixture f;
  codex_llm_client client(f.accounts, key, options(), f.http);
  f.http->push(tool_stream(), 7);
  std::string args, summary;
  std::vector<llm_stream_event_type> events;
  auto result = client.complete_stream(request(true),
    { .on_event =
        [&](const auto& e) {
          events.push_back(e.type);
          if (e.tool_call_delta)
            args += e.tool_call_delta->arguments_delta;
        },
      .on_reasoning_delta = [&](std::string_view text) { summary += text; } });
  require(!result.error_code && result.tool_calls.size() == 1 &&
            result.tool_calls[0].id == "call-1" && args == R"({"text":"world"})" &&
            summary == "Plan." && result.provider_state,
    "tool/reasoning mapping failed");
  require(events[events.size() - 2] == llm_stream_event_type::tool_call_done &&
            events.back() == llm_stream_event_type::done,
    "tool committed before terminal response");
  require(result.content.find("opaque") == std::string::npos &&
            result.reasoning_summary.find("opaque") == std::string::npos,
    "encrypted state exposed as content");
  auto next = request(true);
  next.messages.push_back({ .role = "assistant",
    .tool_calls = result.tool_calls,
    .provider_state = result.provider_state });
  next.messages.push_back({ .role = "tool", .content = "world", .tool_call_id = "call-1" });
  // This is also the serialization path used by durable approvals.
  const auto stored = agent::runtime::llm_codec::request_to_json(next);
  next = agent::runtime::llm_codec::request_from_json(stored);
  f.http->push(text_stream("finished"));
  require(!client.complete(next).error_code, "state replay failed");
  const auto payload = json::parse(f.http->requests.back().body);
  require(payload["input"][1]["type"] == "reasoning" &&
            payload["input"][1]["encrypted_content"] == "opaque-encrypted-state" &&
            payload["input"][2]["type"] == "function_call" &&
            payload["input"][3]["type"] == "function_call_output" &&
            payload["input"][3]["call_id"] == "call-1",
    "tool history order or state lost");
  auto altered = json::parse(next.messages[2].provider_state->data);
  altered["workspace"] = "other";
  next.messages[2].provider_state->data = altered.dump();
  const auto calls = f.http->requests.size();
  require(client.complete(next).error_code == llm_error_code::invalid_request &&
            f.http->requests.size() == calls,
    "cross-account state replay allowed");
}
void test_ordered_phase_replay() {
  fixture f;
  codex_llm_client client(f.accounts, key, options(), f.http);
  auto commentary = message("Checking.");
  commentary.erase("id");
  commentary["phase"] = "commentary";
  auto answer = message("Ready.");
  answer.erase("id");
  answer["phase"] = "final_answer";
  auto thought = reasoning();
  thought.erase("id");
  thought["content"] = json::array({ { { "type", "reasoning_text" }, { "text", "hidden" } } });
  f.http->push(event(completed(json::array({ commentary, thought, answer, call() }))));
  auto result = client.complete(request(true));
  require(!result.error_code && result.content == "Checking.Ready." && result.provider_state,
    "multi-message response failed");
  require(result.provider_state->data.find("hidden") == std::string::npos,
    "hidden reasoning plaintext retained");
  auto next = request(true);
  next.messages.push_back({ .role = "assistant",
    .content = result.content,
    .tool_calls = result.tool_calls,
    .provider_state = result.provider_state });
  next.messages.push_back({ .role = "tool", .content = "world", .tool_call_id = "call-1" });
  // Formatting normalization must not invalidate otherwise identical arguments.
  next.messages[2].tool_calls[0].arguments_json = "{ \"text\" : \"world\" }";
  f.http->push(text_stream());
  require(!client.complete(next).error_code, "phase replay failed");
  const auto input = json::parse(f.http->requests.back().body)["input"];
  require(input.size() == 6 && input[1]["phase"] == "commentary" &&
            input[2]["type"] == "reasoning" && !input[2].contains("content") &&
            input[3]["phase"] == "final_answer" && input[4]["type"] == "function_call" &&
            input[5]["type"] == "function_call_output",
    "phase, native ordering or tool identity lost");
  const auto calls = f.http->requests.size();
  next.messages[2].content = "edited";
  require(client.complete(next).error_code == llm_error_code::invalid_request &&
            f.http->requests.size() == calls,
    "mismatched message and protocol state accepted");
  next.messages[2].content = result.content;
  next.messages[2].tool_calls[0].arguments_json = R"({"text":"edited"})";
  require(client.complete(next).error_code == llm_error_code::invalid_request &&
            f.http->requests.size() == calls,
    "mismatched tool and protocol state accepted");
}
class echo_provider {
public:
  int calls = 0;
  std::vector<llm_tool> tools() const {
    return { tool() };
  }
  llm_tool_result invoke(const std::string& name, const std::string& args, std::stop_token) {
    require(name == "echo", "wrong tool invocation");
    ++calls;
    return { .content = json::parse(args)["text"].get<std::string>() };
  }
};
void test_runner_and_durable_approval() {
  for (bool suspended : { false, true }) {
    fixture f;
    codex_llm_client client(f.accounts, key, options(), f.http);
    auto provider = std::make_shared<echo_provider>();
    llm_agent_runner runner(client, provider);
    f.http->push(tool_stream());
    f.http->push(text_stream("finished"));
    llm_agent_run_options run;
    if (suspended) {
      auto store = std::make_shared<agent::runtime::in_memory_agent_run_store>();
      run.runtime = std::make_shared<agent::runtime::agent_run_runtime>(store);
      run.callbacks.authorize_tool_call = [](const auto&) {
        return llm_tool_authorization { .kind = llm_tool_authorization_kind::suspend,
          .reason = "Test approval" };
      };
    }
    auto result = runner.complete(request(), run);
    if (suspended) {
      require(result.error_code == llm_error_code::approval_required && provider->calls == 0,
        "tool executed before approval");
      const auto run_id = result.metadata.at("run_id"),
                 token = result.metadata.at("continuation_token");
      const auto waiting = run.runtime->get(run_id);
      const auto continuation =
        agent::runtime::llm_continuation_from_json(waiting->suspension->continuation);
      require(continuation.request.messages.back().provider_state.has_value(),
        "approval dropped provider state");
      const auto approved = run.runtime->resolve_approval(run_id,
        waiting->revision,
        token,
        agent::runtime::approval_resolution::approved,
        "approved");
      require(static_cast<bool>(approved), "approval failed");
      result = runner.resume(run_id, approved.revision, token, run);
    }
    if (result.error_code)
      std::cerr << "runner suspended=" << suspended << " error=" << result.error_code.message()
                << " content=" << result.content << " HTTP calls=" << f.http->requests.size()
                << '\n';
    require(!result.error_code && result.content == "finished" && provider->calls == 1 &&
              f.http->requests.size() == 2,
      "Agent tool roundtrip failed");
    const auto body = json::parse(f.http->requests.back().body);
    require(body["input"][1]["encrypted_content"] == "opaque-encrypted-state" &&
              body["input"].back()["output"] == "world",
      "Agent lost reasoning or tool result");
  }
}
void test_failures_never_commit_tools() {
  std::vector<std::pair<std::string, llm_error_code>> cases;
  cases.emplace_back(
    event({ { "type", "response.output_item.done" }, { "output_index", 0 }, { "item", call() } }),
    llm_error_code::invalid_response);
  cases.emplace_back("data: [DONE]\n\n", llm_error_code::invalid_response);
  cases.emplace_back("data: {broken}\n\n", llm_error_code::invalid_response);
  cases.emplace_back(
    event({ { "type", "response.failed" },
      { "response",
        { { "error",
          { { "code", "rate_limit_exceeded" }, { "message", "secret-diagnostic" } } } } } }),
    llm_error_code::rate_limited);
  cases.emplace_back(event({ { "type", "response.incomplete" },
                       { "response", { { "output", json::array({ call() }) } } } }),
    llm_error_code::api_error);
  cases.emplace_back(event(completed(json::array({ call("bad", "not-json") }))),
    llm_error_code::invalid_tool_arguments);
  cases.emplace_back(
    event(completed(json::array({ call(), call() }))), llm_error_code::invalid_response);
  auto unknown = call();
  unknown["name"] = "undeclared";
  cases.emplace_back(event(completed(json::array({ unknown }))), llm_error_code::invalid_response);
  auto huge = completed(json::array({ message() }));
  huge["response"]["usage"]["total_tokens"] = 9'999'999'999ULL;
  cases.emplace_back(event(huge), llm_error_code::invalid_response);
  for (const auto& [stream, error] : cases) {
    fixture f;
    codex_llm_client client(f.accounts, key, options(), f.http);
    f.http->push(stream);
    int done = 0, errors = 0, ready = 0;
    const auto result = client.complete_stream(request(true), { .on_event = [&](const auto& e) {
      done += e.type == llm_stream_event_type::done;
      errors += e.type == llm_stream_event_type::error;
      ready += e.type == llm_stream_event_type::tool_call_done;
      require(e.message.find("secret-diagnostic") == std::string::npos, "upstream error leaked");
    } });
    require(result.error_code == error && result.tool_calls.empty() && !result.provider_state &&
              done == 0 && errors == 1 && ready == 0,
      "invalid stream committed tools or wrong error");
  }
}
void test_preflight_and_limits() {
  fixture f;
  codex_llm_client client(f.accounts, key, options(), f.http);
  for (int n = 0; n < 6; ++n) {
    auto r = request();
    if (n == 0)
      r.max_output_tokens = 100;
    if (n == 1)
      r.temperature = 0.7;
    if (n == 2)
      r.stop_sequences = { "END" };
    if (n == 3)
      r.seed = 1;
    if (n == 4)
      r.cache_mode = llm_cache_mode::enabled;
    if (n == 5)
      r.thinking_mode = llm_thinking_mode::disabled;
    require(client.complete(r).error_code == llm_error_code::unsupported_capability &&
              f.http->requests.empty(),
      "unsupported parameter silently dropped");
  }
  auto dangling = request();
  dangling.messages.push_back({ .role = "tool", .content = "orphan", .tool_call_id = "missing" });
  require(client.complete(dangling).error_code == llm_error_code::invalid_request,
    "orphan tool result allowed");
  auto config = options();
  config.max_event_bytes = 40;
  codex_llm_client bounded(f.accounts, key, config, f.http);
  f.http->push("data: " + std::string(100, 'x'), 1);
  require(bounded.complete(request()).error_code == llm_error_code::response_limit_exceeded,
    "unfinished event unbounded");
  config.max_response_bytes = 80;
  config.max_event_bytes = 40;
  codex_llm_client limited(f.accounts, key, config, f.http);
  f.http->push(std::string(100, '\n'), 1);
  require(limited.complete(request()).error_code == llm_error_code::response_limit_exceeded,
    "stream byte limit ignored");
  config = options();
  config.max_output_items = 1;
  codex_llm_client items(f.accounts, key, config, f.http);
  f.http->push(tool_stream());
  require(static_cast<bool>(items.complete(request(true)).error_code), "output item limit ignored");
  auto schema = request();
  schema.json_schema_output = llm_json_schema_output {
    "result", { { "type", "object" }, { "properties", json::object() } }, true
  };
  f.http->push(text_stream("{}"));
  require(!client.complete(schema).error_code, "structured output failed");
  require(json::parse(f.http->requests.back().body)["text"]["format"]["type"] == "json_schema",
    "structured output mapping wrong");
}
void test_cancel_account_fence_and_callback_exception() {
  for (int action = 0; action < 3; ++action) {
    fixture f;
    codex_llm_client client(f.accounts, key, options(), f.http);
    f.http->push(text_stream(), 5);
    std::stop_source stop;
    int done = 0;
    bool acted = false;
    const auto result = client.complete_stream(request(),
      { .on_event =
          [&](const auto& e) {
            done += e.type == llm_stream_event_type::done;
            if (e.type != llm_stream_event_type::content_delta || acted)
              return;
            acted = true;
            if (action == 0)
              stop.request_stop();
            else if (action == 1)
              require(!f.accounts.remove_account(key), "logout failed");
            else
              require(!f.accounts.save_authorization(info(), tokens()), "relogin failed");
          } },
      stop.get_token());
    require(done == 0 && (action == 0 ? result.error_code == llm_error_code::cancelled
                                      : result.error_code == oauth_error::account_changed),
      "late response crossed cancellation/account boundary");
  }
  fixture f;
  codex_llm_client client(f.accounts, key, options(), f.http);
  f.http->push(text_stream());
  bool threw = false;
  try {
    client.complete_stream(request(),
      { .on_event = [](const auto&) { throw std::runtime_error("application callback"); } });
  }
  catch (const std::runtime_error& e) {
    threw = std::string(e.what()) == "application callback";
  }
  require(threw && f.http->requests.size() == 1, "callback exception swallowed or retried");
}
void test_deadlines_interrupt_silent_transport() {
  for (bool idle : { false, true }) {
    fixture f;
    auto config = options();
    config.timeout_ms = 3000;
    if (idle)
      config.stream_timeouts.idle_ms = 30;
    else
      config.stream_timeouts.first_event_ms = 30;
    codex_llm_client client(f.accounts, key, config, f.http);
    f.http->steps.push_back([idle](const auto&, const auto& chunk, std::stop_token stop) {
      if (idle)
        chunk(event({ { "type", "response.created" }, { "response", { { "id", "resp-1" } } } }));
      std::mutex mutex;
      std::condition_variable_any changed;
      std::unique_lock lock(mutex);
      changed.wait_for(lock, stop, 2s, [] { return false; });
      return http_response { .error_code = transport_error::aborted_by_callback,
        .status_code = 200 };
    });
    const auto begin = std::chrono::steady_clock::now();
    auto result = client.complete(request());
    require(result.error_code == llm_error_code::timeout &&
              result.metadata["timeout_phase"] == (idle ? "idle" : "first_event") &&
              std::chrono::steady_clock::now() - begin < 1500ms,
      "phase timeout did not interrupt silent I/O");
  }
}
void test_status_and_transport_failures() {
  for (const auto& [status, error] :
    std::vector<std::pair<int, llm_error_code>> { { 401, llm_error_code::authentication_failed },
      { 403, llm_error_code::authentication_failed },
      { 429, llm_error_code::rate_limited },
      { 400, llm_error_code::invalid_request },
      { 503, llm_error_code::http_error } }) {
    fixture f;
    codex_llm_client client(f.accounts, key, options(), f.http);
    f.http->steps.push_back([status](const auto&, const auto&, auto) {
      return http_response { .status_code = status, .body = "secret error body" };
    });
    auto result = client.complete(request());
    require(result.error_code == error && result.content.empty() && f.http->requests.size() == 1,
      "HTTP error leaked or retried");
  }
  fixture f;
  codex_llm_client client(f.accounts, key, options(), f.http);
  f.http->steps.push_back([](const auto&, const auto& chunk, auto) {
    chunk(event({ { "type", "response.output_text.delta" },
      { "output_index", 0 },
      { "item_id", "msg-1" },
      { "delta", "partial" } }));
    return http_response { .transport_error = transport_error::recv_error, .status_code = 200 };
  });
  auto result = client.complete(request());
  require(result.error_code == llm_error_code::transport_error && result.content == "partial" &&
            f.http->requests.size() == 1,
    "partial generation was retried");
}
void test_sse_bounds_and_framing() {
  sse_event_parser parser(100);
  std::vector<sse_event> events;
  auto emit = [&](const sse_event& e) {
    events.push_back(e);
    return true;
  };
  require(
    parser.feed("event: test\rdata: first\r", emit) && parser.feed("\ndata: second\r\r", emit),
    "CR framing failed");
  require(events.size() == 1 && events[0].data == "first\nsecond", "multiline SSE failed");
  require(!parser.feed(std::string(101, 'x'), emit) && parser.limit_exceeded(),
    "line limit not enforced");
  parser.reset();
  require(
    parser.feed("data: ok\n\n", emit) && !parser.limit_exceeded(), "bounded parser reset failed");
}

void test_generation_refresh_and_concurrent_calls() {
  class generation_refresher final : public oauth_token_refresher {
  public:
    int calls = 0;
    bool reject = false;
    std::string provider_id() const override {
      return "codex_oauth";
    }
    oauth_refresh_result refresh(
      const oauth_account_info&, const oauth_tokens&, std::stop_token) override {
      ++calls;
      if (reject)
        return { .error = oauth_error::reauthentication_required };
      auto renewed = tokens();
      renewed.access_token = "renewed-access";
      return { renewed, {} };
    }
  };
  auto refreshing = std::make_shared<generation_refresher>();
  oauth_account_manager accounts(make_memory_oauth_credential_store(), { refreshing });
  auto old = tokens();
  old.expires_at = std::chrono::system_clock::now() + 1s;
  require(!accounts.save_authorization(info(), old), "refresh setup failed");
  auto http = std::make_shared<scripted_http>();
  codex_llm_client client(accounts, key, options(), http);
  http->push(text_stream());
  require(
    !client.complete(request()).error_code && refreshing->calls == 1, "generation did not refresh");
  require(
    http->requests[0].headers[0].second == "Bearer renewed-access", "generation used stale token");
  refreshing->reject = true;
  require(!accounts.save_authorization(info(), old), "refresh reset failed");
  require(client.complete(request()).error_code == oauth_error::reauthentication_required &&
            http->requests.size() == 1,
    "failed refresh still sent generation");

  class concurrent_http final : public http_client {
    std::latch entered_ { 2 };

  public:
    http_response send(const http_request&) override {
      return {};
    }
    http_response send_stream(
      const http_request& req, const http_stream_chunk_callback& chunk, std::stop_token) override {
      const auto prompt =
        json::parse(req.body)["input"][0]["content"][0]["text"].get<std::string>();
      entered_.count_down();
      entered_.wait();
      const auto body = text_stream(prompt);
      for (char c : body)
        if (!chunk(std::string_view(&c, 1)))
          break;
      return { .status_code = 200 };
    }
  };
  require(!accounts.save_authorization(info(), tokens()), "concurrent setup failed");
  codex_llm_client parallel(accounts, key, options(), std::make_shared<concurrent_http>());
  auto one = request();
  one.messages.back().content = "one";
  auto two = request();
  two.messages.back().content = "two";
  auto first = std::async(std::launch::async, [&] { return parallel.complete(one); });
  auto second = std::async(std::launch::async, [&] { return parallel.complete(two); });
  const auto a = first.get(), b = second.get();
  require(!a.error_code && !b.error_code && a.content == "one" && b.content == "two",
    "concurrent streams shared state");
}
} // namespace
int main() {
  try {
    test_request_and_text_stream();
    test_tool_stream_and_state();
    test_ordered_phase_replay();
    test_runner_and_durable_approval();
    test_failures_never_commit_tools();
    test_preflight_and_limits();
    test_cancel_account_fence_and_callback_exception();
    test_deadlines_interrupt_silent_transport();
    test_status_and_transport_failures();
    test_sse_bounds_and_framing();
    test_generation_refresh_and_concurrent_calls();
    std::cout << "Codex generation tests passed\n";
    return 0;
  }
  catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
