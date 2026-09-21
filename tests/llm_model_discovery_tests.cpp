#include <wuwe/agent/llm/llm_model_discovery.h>

#include <chrono>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <utility>

#include <httplib/httplib.h>

#include <wuwe/net/cpr_http_client.h>
#include <wuwe/net/http_client.h>
#include <wuwe/net/http_status_code.h>
#include <wuwe/net/httplib_http_client.h>
#include <wuwe/net/transport_error.h>

namespace {

using wuwe::agent::llm_error_code;

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

class scripted_http final : public wuwe::http_client {
public:
  std::vector<wuwe::http_response> responses;
  std::vector<wuwe::http_request> requests;
  std::function<void(std::stop_token)> before_response;

  wuwe::http_response send(const wuwe::http_request&) override {
    throw std::runtime_error("discovery must use cancellable transport");
  }

  wuwe::http_response send_stream(const wuwe::http_request& request,
    const wuwe::http_stream_chunk_callback& callback, std::stop_token stop) override {
    requests.push_back(request);
    require(requests.size() <= responses.size(), "unexpected HTTP request");
    if (before_response) {
      before_response(stop);
    }
    auto response = responses[requests.size() - 1];
    // Deliberately split JSON tokens, including UTF-8, across callbacks.
    for (std::size_t i = 0; i < response.body.size(); i += 3) {
      if (!callback(std::string_view(response.body).substr(i, 3))) {
        response.transport_error =
          wuwe::make_error_code(wuwe::transport_error::aborted_by_callback);
        response.error_code = response.transport_error;
        break;
      }
    }
    return response;
  }

  void add(std::string body, int status = 200) {
    responses.push_back({
      .error_code = wuwe::make_error_code(static_cast<wuwe::http_status_code>(status)),
      .status_code = status,
      .body = std::move(body),
    });
  }
};

wuwe::llm_client_config config() {
  return { .api_key = "test-key", .load_api_key_from_environment = false };
}

std::string header(const wuwe::http_request& request, std::string_view name) {
  for (const auto& [key, value] : request.headers) {
    if (wuwe::http_header_name_equals(key, name)) {
      return value;
    }
  }
  return {};
}

void expect_error(const wuwe::llm_model_list_result& result, llm_error_code code) {
  require(result.error_code == wuwe::agent::make_error_code(code), "wrong error classification");
  require(result.models.empty(), "failure leaked a partial model list");
}

void test_provider_endpoints() {
  const std::pair<const char*, const char*> providers[] {
    { "OpenAI", "https://api.openai.com/v1/models" },
    { "OpenRouter", "https://openrouter.ai/api/v1/models" },
    { "DeepSeek", "https://api.deepseek.com/v1/models" },
    { "DashScope", "https://dashscope.aliyuncs.com/compatible-mode/v1/models" },
    { "Qwen", "https://dashscope.aliyuncs.com/compatible-mode/v1/models" },
    { "Zhipu", "https://open.bigmodel.cn/api/paas/v4/models" },
    { "Kimi", "https://api.moonshot.cn/v1/models" },
    { "MiniMax", "https://api.minimaxi.com/v1/models" },
    { "SiliconFlow", "https://api.siliconflow.cn/v1/models" },
    { "Doubao", "https://ark.cn-beijing.volces.com/api/v3/models" },
    { "Nvidia", "https://integrate.api.nvidia.com/v1/models" },
    { "StepFun", "https://api.stepfun.com/v1/models" },
    { "MiMo", "https://api.xiaomimimo.com/v1/models" },
  };
  for (const auto& [provider, url] : providers) {
    scripted_http http;
    http.add(
      R"({"data":[{"id":"vendor/model:latest"},{"id":"vendor/model:latest"},{"id":"second"}]})");
    const auto result = wuwe::list_llm_models(provider, config(), http);
    require(!result.error_code && result.models.size() == 2, "compatible provider failed");
    require(result.models[0].id == "vendor/model:latest" && result.models[1].id == "second",
      "IDs or order changed");
    require(!result.models[0].display_name, "display name was invented");
    require(http.requests[0].url == url, "wrong preset URL");
    require(http.requests[0].method == "GET" && http.requests[0].body.empty(),
      "discovery invoked generation");
    require(header(http.requests[0], "Authorization") == "Bearer test-key", "wrong bearer auth");
    require(!http.requests[0].follow_redirects, "credentials may follow redirects");
    require(http.requests[0].timeout > 0 && http.requests[0].timeout <= config().timeout,
      "timeout missing");
  }
  for (const std::string base :
    { "https://gateway.test/v1/", "https://gateway.test/v4", "https://gateway.test/prefix" }) {
    auto cfg = config();
    cfg.base_url = base;
    scripted_http http;
    http.add(R"({"data":[]})");
    const auto result = wuwe::list_llm_models("OpenAICompatible", cfg, http);
    require(!result.error_code && result.models.empty(), "successful empty list failed");
    const auto expected = base.ends_with("v1/")  ? "https://gateway.test/v1/models"
                          : base.ends_with("v4") ? "https://gateway.test/v4/models"
                                                 : "https://gateway.test/prefix/v1/models";
    require(http.requests[0].url == expected, "wrong versioned base URL");
  }
}

void test_formats_and_pagination() {
  {
    scripted_http http;
    http.add(
      R"({"data":[{"id":"a","display_name":"Claude A"}],"has_more":true,"last_id":"a+/&?"})");
    http.add(R"({"data":[{"id":"a"},{"id":"b"}],"has_more":false})");
    const auto result = wuwe::list_llm_models("Anthropic", config(), http);
    require(!result.error_code && result.models.size() == 2, "Anthropic pagination failed");
    require(result.models[0].display_name == "Claude A", "first metadata not retained");
    require(http.requests[1].url == "https://api.anthropic.com/v1/models?after_id=a%2B%2F%26%3F",
      "unsafe Anthropic cursor");
    require(header(http.requests[0], "x-api-key") == "test-key" &&
              header(http.requests[0], "anthropic-version") == "2023-06-01" &&
              header(http.requests[0], "Authorization").empty(),
      "wrong Anthropic auth");
  }
  {
    scripted_http http;
    http.add(
      R"({"models":[{"name":"models/gemini-test","displayName":"Gemini \u6d4b\u8bd5"}],"nextPageToken":"x=y&z"})");
    http.add(R"({"models":[{"name":"models/embedding-test"}]})");
    const auto result = wuwe::list_llm_models("Gemini", config(), http);
    require(!result.error_code && result.models.size() == 2 && result.models[0].id == "gemini-test",
      "Gemini IDs failed");
    require(result.models[1].id == "embedding-test", "discovery silently filtered models");
    require(result.models[0].display_name.has_value(), "Gemini display name missing");
    require(http.requests[1].url ==
              "https://generativelanguage.googleapis.com/v1beta/models?pageToken=x%3Dy%26z",
      "unsafe Gemini cursor");
    require(header(http.requests[0], "x-goog-api-key") == "test-key" &&
              http.requests[0].url.find("test-key") == std::string::npos,
      "Gemini key in URL");
    scripted_http empty;
    empty.add("{}");
    require(
      !wuwe::list_llm_models("Gemini", config(), empty).error_code, "empty protobuf list rejected");
  }
  {
    scripted_http http;
    http.add(R"({"models":[{"name":"llama:latest"}]})");
    auto cfg = config();
    cfg.api_key.clear();
    const auto result = wuwe::list_llm_models("Ollama", cfg, http);
    require(!result.error_code && result.models[0].id == "llama:latest", "Ollama failed");
    require(http.requests[0].url == "http://localhost:11434/api/tags" &&
              header(http.requests[0], "Authorization").empty(),
      "Ollama endpoint/auth failed");
  }
  {
    scripted_http http;
    http.add(R"({"data":[{"id":"a"}],"has_more":true,"last_id":"a"})");
    http.add(R"({"data":[{"id":"b"}]})");
    const auto result = wuwe::list_llm_models("OpenAI", config(), http);
    require(
      !result.error_code && result.models.size() == 2 && http.requests[1].url.ends_with("?after=a"),
      "OpenAI pagination failed");
  }
}

void test_endpoint_path_boundaries() {
  struct endpoint_case {
    const char* provider;
    const char* base;
    const char* expected;
    const char* body;
  };
  const endpoint_case cases[] {
    { "OpenAI", "https://v1", "https://v1/v1/models", R"({"data":[]})" },
    { "Anthropic", "https://v1", "https://v1/v1/models", R"({"data":[]})" },
    { "Gemini", "https://v1beta", "https://v1beta/v1beta/models", R"({"models":[]})" },
    { "Ollama", "http://api", "http://api/api/tags", R"({"models":[]})" },
    { "Anthropic", "https://host/prefix/v1/", "https://host/prefix/v1/models", R"({"data":[]})" },
    { "Gemini",
      "https://host/prefix/v1beta/",
      "https://host/prefix/v1beta/models",
      R"({"models":[]})" },
    { "Ollama", "http://host/prefix/api/", "http://host/prefix/api/tags", R"({"models":[]})" },
  };
  for (const auto& item : cases) {
    scripted_http http;
    http.add(item.body);
    auto cfg = config();
    cfg.base_url = item.base;
    require(!wuwe::list_llm_models(item.provider, cfg, http).error_code, "endpoint query failed");
    require(http.requests[0].url == item.expected, "hostname mistaken for endpoint path");
  }
  scripted_http custom;
  custom.add(R"({"data":[]})");
  auto cfg = config();
  cfg.base_url = "https://host/prefix";
  cfg.chat_completions_path = "/api/v2/chat/completions";
  require(!wuwe::list_llm_models("OpenAICompatible", cfg, custom).error_code,
    "custom chat path rejected");
  require(
    custom.requests[0].url == "https://host/prefix/api/v2/models", "custom chat sibling path lost");
}

void test_custom_endpoint_and_validation() {
  scripted_http http;
  http.add(R"({"data":[{"id":"custom"}]})");
  auto cfg = config();
  cfg.base_url = "https://gateway.test/";
  const auto result = wuwe::list_llm_models("Anthropic",
    cfg,
    http,
    { .format = wuwe::llm_model_list_format::openai, .models_path = "/openai/v1/models" });
  require(!result.error_code && http.requests[0].url == "https://gateway.test/openai/v1/models",
    "custom discovery route failed");
  require(header(http.requests[0], "Authorization") == "Bearer test-key" &&
            header(http.requests[0], "x-api-key").empty(),
    "format override did not select auth");

  scripted_http none;
  expect_error(wuwe::list_llm_models("Unknown", config(), none), llm_error_code::invalid_request);
  expect_error(
    wuwe::list_llm_models("OpenAICompatible", config(), none), llm_error_code::invalid_request);
  for (const std::string base : { "file:///tmp/models",
         "https://",
         "https://key@host",
         "https://host?key=secret",
         "https://host/#fragment",
         "https://host\r\nInjected: x" }) {
    cfg.base_url = base;
    expect_error(wuwe::list_llm_models("OpenAI", cfg, none), llm_error_code::invalid_request);
  }
  for (const std::string path : { "https://other.test/models",
         "//other.test/models",
         "/models?key=secret",
         "/models#x",
         "/models\r\n" }) {
    expect_error(wuwe::list_llm_models("OpenAI", config(), none, { .models_path = path }),
      llm_error_code::invalid_request);
  }
  cfg = config();
  cfg.api_key.clear();
  expect_error(wuwe::list_llm_models("OpenAI", cfg, none), llm_error_code::missing_api_key);
  cfg.api_key = "key\r\nInjected: x";
  expect_error(wuwe::list_llm_models("OpenAI", cfg, none), llm_error_code::invalid_request);
  cfg = config();
  cfg.timeout = 0;
  expect_error(wuwe::list_llm_models("OpenAI", cfg, none), llm_error_code::invalid_request);
  expect_error(wuwe::list_llm_models("OpenAI", config(), none, { .max_pages = 0 }),
    llm_error_code::invalid_request);
  expect_error(wuwe::list_llm_models("OpenAI", config(), none, { .max_models = 0 }),
    llm_error_code::invalid_request);
  expect_error(wuwe::list_llm_models("OpenAI", config(), none, { .max_response_bytes = 0 }),
    llm_error_code::invalid_request);
  expect_error(
    wuwe::list_llm_models(
      "OpenAI", config(), none, { .format = static_cast<wuwe::llm_model_list_format>(99) }),
    llm_error_code::invalid_request);
  cfg = config();
  cfg.chat_completions_path = "/generate";
  expect_error(wuwe::list_llm_models("OpenAI", cfg, none), llm_error_code::invalid_request);
  require(none.requests.empty(), "invalid input reached transport");
}

void test_errors_and_atomic_results() {
  const std::pair<int, llm_error_code> statuses[] {
    { 301, llm_error_code::http_error },
    { 400, llm_error_code::http_error },
    { 401, llm_error_code::authentication_failed },
    { 403, llm_error_code::authentication_failed },
    { 404, llm_error_code::unsupported_capability },
    { 405, llm_error_code::unsupported_capability },
    { 408, llm_error_code::timeout },
    { 429, llm_error_code::rate_limited },
    { 500, llm_error_code::http_error },
    { 501, llm_error_code::unsupported_capability },
    { 504, llm_error_code::timeout },
  };
  for (const auto& [status, code] : statuses) {
    scripted_http http;
    http.add(R"({"data":[{"id":"first"}],"has_more":true,"last_id":"first"})");
    http.add("upstream reflected secret test-key", status);
    const auto result = wuwe::list_llm_models("OpenAI", config(), http);
    expect_error(result, code);
    require(result.http_status == status && !result.transport_error, "HTTP metadata lost");
    require(result.error_code.message().find("test-key") == std::string::npos, "secret leaked");
    require(http.requests.size() == 2, "unexpected retry or probing");
  }
  for (const auto transport :
    { wuwe::transport_error::operation_timedout, wuwe::transport_error::couldnt_connect }) {
    scripted_http http;
    http.responses.push_back({ .error_code = wuwe::make_error_code(transport),
      .transport_error = wuwe::make_error_code(transport) });
    const auto result = wuwe::list_llm_models("OpenAI", config(), http);
    expect_error(result,
      transport == wuwe::transport_error::operation_timedout ? llm_error_code::timeout
                                                             : llm_error_code::transport_error);
    require(result.transport_error == wuwe::make_error_code(transport), "transport detail lost");
  }
  for (const std::string body : { "",
         "<html>error</html>",
         "[]",
         "{}",
         R"({"data":{}})",
         R"({"data":[{}]})",
         R"({"data":[{"id":1}]})",
         R"({"data":[{"id":""}]})",
         R"({"data":[{"id":"x\n"}]})",
         R"({"data":[{"id":"x","display_name":3}]})",
         R"({"data":[],"has_more":true})",
         R"({"data":[],"has_more":"true"})" }) {
    scripted_http http;
    http.add(body);
    expect_error(wuwe::list_llm_models("OpenAI", config(), http), llm_error_code::invalid_response);
  }
  scripted_http api_error;
  api_error.add(R"({"error":{"message":"test-key"}})");
  expect_error(wuwe::list_llm_models("OpenAI", config(), api_error), llm_error_code::api_error);

  scripted_http optional_names;
  optional_names.add(
    R"({"data":[{"id":"a","display_name":null},{"id":"b","name":"Friendly name"},{"id":"c","display_name":"Preferred","name":"Fallback"}]})");
  const auto names = wuwe::list_llm_models("OpenAI", config(), optional_names);
  require(!names.error_code && !names.models[0].display_name &&
            names.models[1].display_name == "Friendly name" &&
            names.models[2].display_name == "Preferred",
    "optional provider display names were not handled correctly");
  scripted_http gemini_null_name;
  gemini_null_name.add(R"({"models":[{"name":"models/a","displayName":null}]})");
  require(!wuwe::list_llm_models("Gemini", config(), gemini_null_name).error_code,
    "nullable display metadata rejected a valid model");

  const std::pair<const char*, const char*> invalid_pages[] {
    { "Gemini", R"({"message":"unexpected upstream response"})" },
    { "Gemini", R"({"models":[{"name":"models/"}]})" },
    { "Gemini", R"({"models":[],"nextPageToken":42})" },
    { "Gemini", R"({"models":null})" },
    { "Ollama", R"({"models":[{"model":"missing-name"}]})" },
    { "Anthropic", R"({"data":[{"id":"a"}],"has_more":true,"last_id":null})" },
  };
  for (const auto& [provider, body] : invalid_pages) {
    scripted_http http;
    http.add(body);
    expect_error(wuwe::list_llm_models(provider, config(), http), llm_error_code::invalid_response);
  }
}

void test_limits_and_cancellation() {
  const std::string paged = R"({"data":[{"id":"x"}],"has_more":true,"last_id":"x"})";
  scripted_http cycle;
  cycle.add(paged);
  cycle.add(paged);
  expect_error(wuwe::list_llm_models("OpenAI", config(), cycle), llm_error_code::invalid_response);
  require(cycle.requests.size() == 2, "pagination cycle not stopped");
  scripted_http pages;
  pages.add(paged);
  expect_error(wuwe::list_llm_models("OpenAI", config(), pages, { .max_pages = 1 }),
    llm_error_code::model_list_limit_exceeded);
  scripted_http models;
  models.add(R"({"data":[{"id":"a"},{"id":"b"}]})");
  expect_error(wuwe::list_llm_models("OpenAI", config(), models, { .max_models = 1 }),
    llm_error_code::model_list_limit_exceeded);
  scripted_http bytes;
  bytes.add(R"({"data":[]})");
  expect_error(wuwe::list_llm_models("OpenAI", config(), bytes, { .max_response_bytes = 5 }),
    llm_error_code::model_list_limit_exceeded);
  scripted_http exact;
  exact.add(R"({"data":[]})");
  require(
    !wuwe::list_llm_models("OpenAI", config(), exact, { .max_response_bytes = 11 }).error_code,
    "exact byte limit rejected");

  std::stop_source cancelled;
  cancelled.request_stop();
  scripted_http none;
  expect_error(wuwe::list_llm_models("OpenAI", config(), none, {}, cancelled.get_token()),
    llm_error_code::cancelled);
  require(none.requests.empty(), "pre-cancelled operation reached network");
  // Exercise the default-transport overload without performing network I/O.
  expect_error(wuwe::list_llm_models("OpenAI", config(), {}, cancelled.get_token()),
    llm_error_code::cancelled);

  std::stop_source during;
  scripted_http streaming;
  streaming.add(paged);
  streaming.add(R"({"data":[{"id":"b"}]})");
  streaming.before_response = [&](std::stop_token token) {
    require(token.stop_possible(), "stop token not forwarded");
    if (streaming.requests.size() == 2) {
      during.request_stop();
    }
  };
  expect_error(wuwe::list_llm_models("OpenAI", config(), streaming, {}, during.get_token()),
    llm_error_code::cancelled);

  auto cfg = config();
  cfg.timeout = 10;
  scripted_http slow;
  slow.add(R"({"data":[]})");
  slow.before_response = [](std::stop_token) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  };
  expect_error(wuwe::list_llm_models("OpenAI", cfg, slow), llm_error_code::timeout);
}

void test_real_http_backends() {
  httplib::Server server;
  server.Get("/v1/models", [](const httplib::Request& request, httplib::Response& response) {
    if (request.get_header_value("Authorization") != "Bearer test-key") {
      response.status = 401;
      return;
    }
    if (request.has_param("after")) {
      if (request.get_param_value("after") != "a+/&?") {
        response.status = 400;
        return;
      }
      response.set_content(R"({"data":[{"id":"b"}]})", "application/json");
    }
    else {
      response.set_content(
        R"({"data":[{"id":"a"}],"has_more":true,"last_id":"a+/&?"})", "application/json");
    }
  });
  server.Get("/redirect", [](const httplib::Request&, httplib::Response& response) {
    response.status = 302;
    response.set_header("Location", "/v1/models");
  });
  server.Get("/denied", [](const httplib::Request&, httplib::Response& response) {
    response.status = 403;
    response.set_content("test-key", "text/plain");
  });
  const auto port = server.bind_to_any_port("127.0.0.1");
  require(port > 0, "failed to bind loopback test server");
  std::jthread worker([&](std::stop_token stop) {
    std::stop_callback stop_server(stop, [&] { server.stop(); });
    if (!stop.stop_requested()) {
      server.listen_after_bind();
    }
  });
  const auto ready_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!server.is_running()) {
    require(std::chrono::steady_clock::now() < ready_deadline, "loopback server did not start");
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  auto cfg = config();
  cfg.base_url = "http://127.0.0.1:" + std::to_string(port);
  wuwe::cpr_http_client cpr;
  wuwe::httplib_http_client httplib;
  for (auto* http :
    { static_cast<wuwe::http_client*>(&cpr), static_cast<wuwe::http_client*>(&httplib) }) {
    const auto result = wuwe::list_llm_models("OpenAICompatible", cfg, *http);
    require(!result.error_code && result.models.size() == 2, "real transport pagination failed");
    require(result.models[0].id == "a" && result.models[1].id == "b", "real transport IDs changed");
    expect_error(
      wuwe::list_llm_models("OpenAICompatible", cfg, *http, { .models_path = "/redirect" }),
      llm_error_code::http_error);
    expect_error(
      wuwe::list_llm_models("OpenAICompatible", cfg, *http, { .models_path = "/denied" }),
      llm_error_code::authentication_failed);
    expect_error(wuwe::list_llm_models("OpenAICompatible", cfg, *http, { .max_response_bytes = 5 }),
      llm_error_code::model_list_limit_exceeded);
  }
}

} // namespace

int main() {
  try {
    test_provider_endpoints();
    test_formats_and_pagination();
    test_endpoint_path_boundaries();
    test_custom_endpoint_and_validation();
    test_errors_and_atomic_results();
    test_limits_and_cancellation();
    test_real_http_backends();
    std::cout << "llm_model_discovery_tests passed\n";
    return 0;
  }
  catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
