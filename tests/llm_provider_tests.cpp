#include <chrono>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <wuwe/net/http_status_code.h>
#include <wuwe/net/net_errc.h>
#include <wuwe/wuwe.h>

namespace {

struct aggregate_header_echo {
  static constexpr std::string_view description = "Echo text from an aggregate-header test.";

  wuwe::field<std::string> text {
    .description = "Text to echo.",
  };

  std::string invoke() const {
    return text.value;
  }
};

void require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

class capture_http_client final : public wuwe::http_client {
public:
  explicit capture_http_client(std::string body) : responses_({ { .body = std::move(body) } }) {
  }

  explicit capture_http_client(std::vector<wuwe::http_response> responses)
      : responses_(std::move(responses)) {
  }

  wuwe::http_response send(const wuwe::http_request& request) override {
    requests.push_back(request);
    return next_response();
  }

  wuwe::http_response send_stream(const wuwe::http_request& request,
    const wuwe::http_stream_chunk_callback& on_chunk, std::stop_token = {}) override {
    requests.push_back(request);
    auto response = next_response();
    if (!response.error_code && on_chunk && !response.body.empty()) {
      on_chunk(response.body);
    }
    return response;
  }

  std::vector<wuwe::http_request> requests;

private:
  wuwe::http_response next_response() {
    if (next_ >= responses_.size()) {
      return responses_.empty() ? wuwe::http_response {} : responses_.back();
    }
    return responses_[next_++];
  }

  std::vector<wuwe::http_response> responses_;
  std::size_t next_ { 0 };
};

class streaming_error_http_client final : public wuwe::http_client {
public:
  explicit streaming_error_http_client(std::string body) : body_(std::move(body)) {
  }

  wuwe::http_response send(const wuwe::http_request& request) override {
    requests.push_back(request);
    return {};
  }

  wuwe::http_response send_stream(const wuwe::http_request& request,
    const wuwe::http_stream_chunk_callback& on_chunk, std::stop_token = {}) override {
    requests.push_back(request);
    if (on_chunk && !body_.empty()) {
      on_chunk(body_);
    }
    return {
      .error_code = std::make_error_code(std::errc::connection_reset),
      .body = body_,
    };
  }

  std::vector<wuwe::http_request> requests;

private:
  std::string body_;
};

class factory_extension_llm_client final : public wuwe::llm_client {
public:
  explicit factory_extension_llm_client(const wuwe::llm_config& config) : model_(config.model) {
  }

  wuwe::llm_response complete(const wuwe::llm_request&) override {
    return { .content = model_ };
  }

private:
  std::string model_;
};

bool has_request_header(const wuwe::http_request& request, std::string_view name) {
  for (const auto& [key, value] : request.headers) {
    if (wuwe::http_header_name_equals(key, name) && !value.empty()) {
      return true;
    }
  }
  return false;
}

void test_factory_registers_protocol_and_provider_clients() {
  wuwe::llm_client_factory factory;
  auto openai_compatible = factory.create_shared("OpenAICompatible",
    wuwe::llm_client_config {
      .base_url = "https://example.test",
      .api_key = "",
      .require_api_key = false,
      .model = "test-model",
      .max_retries = 0,
    });
  require(
    static_cast<bool>(openai_compatible), "factory should create OpenAICompatible protocol client");
  require(openai_compatible->supports_streaming(),
    "OpenAICompatible protocol client should support streaming");

  auto openrouter = factory.create_shared("OpenRouter",
    wuwe::llm_client_config {
      .api_key = "",
      .require_api_key = false,
      .model = "test-model",
      .max_retries = 0,
    });
  require(static_cast<bool>(openrouter), "factory should create OpenRouter provider preset");
  require(openrouter->supports_streaming(), "OpenRouter provider preset should support streaming");

  auto via_helper = wuwe::make_llm_client("OpenAI",
    wuwe::llm_client_config {
      .api_key = "",
      .require_api_key = false,
      .model = "test-model",
      .max_retries = 0,
    });
  require(static_cast<bool>(via_helper), "make_llm_client should create provider clients");

  auto via_singleton = wuwe::llm_client_factory::instance().create_shared("Gemini",
    wuwe::llm_client_config {
      .api_key = "",
      .require_api_key = false,
      .model = "test-model",
      .max_retries = 0,
    });
  require(static_cast<bool>(via_singleton),
    "llm_client_factory singleton should create provider clients");

  for (const auto* key : {
         "OpenAI",
         "Anthropic",
         "Gemini",
         "Ollama",
         "DeepSeek",
         "DashScope",
         "Qwen",
         "Zhipu",
         "Kimi",
         "MiniMax",
         "SiliconFlow",
         "Doubao",
         "Nvidia",
         "StepFun",
         "MiMo",
       }) {
    auto client = factory.create_shared(key,
      wuwe::llm_client_config {
        .api_key = "",
        .require_api_key = false,
        .model = "test-model",
        .max_retries = 0,
      });
    require(static_cast<bool>(client), std::string("factory should create ") + key);
    require(client->supports_streaming(), std::string(key) + " should support streaming");
  }
}

void test_factory_preserves_gmp_registration_extension() {
  static_assert(std::is_base_of_v<wuwe::llm_client_factory_base, wuwe::llm_client_factory>);
  constexpr auto provider_id = "FactoryExtension";
  wuwe::llm_client_factory factory;
  factory.unregister_type(provider_id);

  {
    wuwe::llm_client_factory::register_type<factory_extension_llm_client> registration(provider_id);
    auto client = factory.create_unique(provider_id,
      wuwe::llm_config {
        .model = "extension-model",
      });
    require(client->complete(wuwe::llm_request {}).content == "extension-model",
      "LLM factory should preserve GMP register_type and create_unique extensions");
  }

  factory.unregister_type(provider_id);
  bool rejected = false;
  try {
    (void)factory.create(provider_id, {});
  }
  catch (const std::invalid_argument&) {
    rejected = true;
  }
  require(rejected, "LLM factory should preserve GMP unregister_type behavior");

  const wuwe::llm_client_factory const_factory;
  auto builtin = const_factory.create_shared("Ollama",
    wuwe::llm_config {
      .model = "const-factory-model",
    });
  require(static_cast<bool>(builtin),
    "LLM factory should preserve const creation supported by the public facade");
}

void test_provider_registry_exposes_default_metadata_and_config() {
  const auto& providers = wuwe::list_llm_providers();
  require(providers.size() >= 10, "provider registry should expose built-in providers");

  const auto* openai = wuwe::find_llm_provider("OpenAI");
  require(openai != nullptr, "provider registry should expose OpenAI");
  require(openai->default_base_url == "https://api.openai.com",
    "OpenAI metadata should expose its default base URL");
  require(openai->default_chat_completions_path == "/v1/chat/completions",
    "OpenAI metadata should expose its default chat completions path");
  require(openai->protocol == wuwe::llm_provider_protocol::openai_compatible,
    "OpenAI metadata should expose its protocol");
  require(
    openai->capabilities.declared && openai->capabilities.streaming && openai->capabilities.tools,
    "OpenAI metadata should expose core chat capabilities");
  require(openai->capabilities.stop_sequences && openai->capabilities.deterministic_seed &&
            openai->capabilities.json_schema_output,
    "OpenAI metadata should expose supported generation controls");
  require(!openai->capabilities.streaming_reasoning_summary,
    "OpenAI chat-completions metadata should not advertise reasoning streams by default");
  require(openai->capabilities.reasoning_language_control ==
            wuwe::llm_reasoning_language_control::unsupported,
    "OpenAI metadata should not advertise reasoning language control without reasoning streams");
  require(wuwe::to_string(openai->protocol) == "openai_compatible",
    "provider protocol should have a stable string form");

  const auto* compatible = wuwe::find_llm_provider("OpenAICompatible");
  require(compatible != nullptr, "provider registry should expose OpenAICompatible");
  require(compatible->default_base_url.empty(),
    "generic OpenAI-compatible metadata should not invent a default base URL");
  require(compatible->base_url_required,
    "generic OpenAI-compatible metadata should mark base URL as required");
  require(compatible->capabilities.stop_sequences && !compatible->capabilities.deterministic_seed &&
            !compatible->capabilities.json_schema_output,
    "generic compatibility metadata should remain conservative");

  const auto* ollama = wuwe::find_llm_provider("Ollama");
  require(ollama != nullptr, "provider registry should expose Ollama");
  require(ollama->default_base_url == "http://localhost:11434",
    "Ollama metadata should expose its local base URL");
  require(!ollama->api_key_required, "Ollama metadata should not require an API key by default");
  require(
    ollama->capabilities.local_runtime, "Ollama metadata should identify local runtime providers");
  require(ollama->capabilities.stop_sequences && ollama->capabilities.deterministic_seed &&
            ollama->capabilities.json_schema_output,
    "Ollama metadata should expose supported generation controls");
  require(ollama->capabilities.streaming_reasoning_summary,
    "Ollama metadata should expose provider-native thinking streams when models return them");
  require(ollama->capabilities.reasoning_language_control ==
            wuwe::llm_reasoning_language_control::prompt_contract,
    "Ollama metadata should not claim reliable native reasoning language control");

  const auto* deepseek = wuwe::find_llm_provider("DeepSeek");
  require(deepseek != nullptr, "provider registry should expose DeepSeek");
  require(deepseek->capabilities.streaming_reasoning_summary,
    "DeepSeek metadata should expose provider-native reasoning streams");
  require(wuwe::to_string(deepseek->capabilities.reasoning_language_control) == "prompt_contract",
    "reasoning language control should have a stable string form");

  const auto anthropic_config = wuwe::make_default_llm_config("Anthropic");
  require(anthropic_config.has_value(),
    "provider registry should build default config for known providers");
  require(anthropic_config->base_url == "https://api.anthropic.com",
    "default config should carry provider base URL");
  require(anthropic_config->chat_completions_path.empty(),
    "native provider default config should not expose an OpenAI chat path");
  require(anthropic_config->require_api_key, "default config should carry provider API-key policy");
  require(anthropic_config->capabilities_override &&
            anthropic_config->capabilities_override->stop_sequences &&
            !anthropic_config->capabilities_override->deterministic_seed,
    "provider configuration should retain capability metadata");

  auto default_normalized = wuwe::normalize_llm_client_config("OpenAI",
    wuwe::llm_client_config {
      .api_key = "explicit",
      .model = "model",
    });
  require(default_normalized.has_value(), "provider registry should normalize OpenAI");
  require(default_normalized->base_url == "https://api.openai.com",
    "normalization should apply provider default base URL");

  auto normalized = wuwe::normalize_llm_client_config("OpenAI",
    wuwe::llm_client_config {
      .base_url = "https://proxy.example",
      .api_key = "explicit",
      .model = "model",
    });
  require(normalized.has_value(), "provider registry should normalize known providers");
  require(normalized->base_url == "https://proxy.example",
    "normalization should preserve explicit base URL overrides");
  require(normalized->api_key == "explicit", "normalization should preserve explicit API keys");

  auto local_normalized = wuwe::normalize_llm_client_config("Ollama",
    wuwe::llm_client_config {
      .model = "llama",
    });
  require(local_normalized.has_value(), "provider registry should normalize Ollama");
  require(!local_normalized->require_api_key, "normalization should apply provider API-key policy");

  const auto* qwen = wuwe::find_llm_provider("Qwen");
  require(qwen != nullptr, "provider registry should expose Qwen");
  require(qwen->api_key_env_names.size() >= 2 && qwen->api_key_env_names[0] == "QWEN_API_KEY",
    "Qwen metadata should prefer QWEN_API_KEY");

  const auto* zhipu = wuwe::find_llm_provider("Zhipu");
  require(zhipu != nullptr, "provider registry should expose Zhipu");
  require(
    zhipu->display_name == "Zhipu GLM", "Zhipu metadata should expose a user-facing display name");
  require(zhipu->default_base_url == "https://open.bigmodel.cn/api/paas/v4",
    "Zhipu metadata should expose the BigModel base URL");
  require(zhipu->default_chat_completions_path == "/chat/completions",
    "Zhipu metadata should expose its non-v1 chat completions path");
  require(zhipu->api_key_env_names.size() >= 2 && zhipu->api_key_env_names[0] == "ZHIPU_API_KEY" &&
            zhipu->api_key_env_names[1] == "BIGMODEL_API_KEY",
    "Zhipu metadata should expose stable API key env names");

  const auto zhipu_config = wuwe::make_default_llm_config("Zhipu");
  require(zhipu_config.has_value(), "provider registry should build default config for Zhipu");
  require(zhipu_config->base_url == "https://open.bigmodel.cn/api/paas/v4",
    "Zhipu default config should carry the BigModel base URL");
  require(zhipu_config->chat_completions_path == "/chat/completions",
    "Zhipu default config should carry the provider-specific chat path");

  require(!wuwe::find_llm_provider("Missing"), "provider registry should report missing providers");
  require(!wuwe::make_default_llm_config("Missing").has_value(),
    "provider registry should not build config for missing providers");
}

void test_aggregate_header_preserves_tool_reflection() {
  const auto tool = wuwe::make_llm_tool<aggregate_header_echo>();
  require(
    tool.name == "aggregate_header_echo", "wuwe.h should not interfere with reflected tool names");
  require(tool.description == aggregate_header_echo::description,
    "wuwe.h should not interfere with reflected tool descriptions");
  require(tool.parameters_json_schema.find("\"text\"") != std::string::npos,
    "wuwe.h should not interfere with reflected tool parameters");
  require(tool.parameters_json_schema.find("Text to echo.") != std::string::npos,
    "wuwe.h should not interfere with field descriptions");
}

std::optional<std::string_view> request_header_value(
  const wuwe::http_request& request, std::string_view name) {
  for (const auto& [key, value] : request.headers) {
    if (wuwe::http_header_name_equals(key, name)) {
      return value;
    }
  }
  return std::nullopt;
}

class scoped_test_env {
public:
  explicit scoped_test_env(const char* name) : name_(name) {
#if defined(_WIN32)
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, name) == 0 && value) {
      previous_ = value;
    }
    std::free(value);
#else
    if (const char* value = std::getenv(name)) {
      previous_ = value;
    }
#endif
  }
  ~scoped_test_env() {
    assign(previous_);
  }
  scoped_test_env(const scoped_test_env&) = delete;
  scoped_test_env& operator=(const scoped_test_env&) = delete;
  void set(std::string value) {
    assign(value);
  }

private:
  void assign(const std::optional<std::string>& value) {
#if defined(_WIN32)
    _putenv_s(name_.c_str(), value ? value->c_str() : "");
#else
    if (value) {
      setenv(name_.c_str(), value->c_str(), 1);
    }
    else {
      unsetenv(name_.c_str());
    }
#endif
  }
  std::string name_;
  std::optional<std::string> previous_;
};

template<typename Client>
void verify_new_provider(
  const char* id, const char* endpoint, const char* key_env, const char* alias_env = nullptr) {
  scoped_test_env primary(key_env);
  scoped_test_env openai("OPENAI_API_KEY");
  scoped_test_env openrouter("OPENROUTER_API_KEY");
  std::optional<scoped_test_env> alias;
  if (alias_env) {
    alias.emplace(alias_env);
    alias->set("");
  }
  primary.set("");
  openai.set("unrelated-openai-key");
  openrouter.set("unrelated-openrouter-key");
  const auto* info = wuwe::find_llm_provider(id);
  require(
    info && info->api_key_env_names.front() == key_env, "dedicated provider metadata missing");
  require(
    info->capabilities.streaming && info->capabilities.tools, "provider lost core capabilities");
  auto cfg = wuwe::llm_client_config { .model = "test-model", .max_retries = 0 };
  const std::string body = R"({"choices":[{"message":{"content":"ok"},"finish_reason":"stop"}]})";
  wuwe::llm_request request;
  request.messages.push_back({ .role = "user", .content = "hello" });
  auto missing_http = std::make_shared<capture_http_client>(body);
  Client missing(cfg, missing_http);
  require(missing.complete(request).error_code == wuwe::agent::llm_error_code::missing_api_key &&
            missing_http->requests.empty(),
    "preset used an unrelated provider key");
  auto factory_missing = wuwe::make_llm_client(id, cfg);
  require(factory_missing && factory_missing->complete(request).error_code ==
                               wuwe::agent::llm_error_code::missing_api_key,
    "factory credential policy differs from direct client");

  primary.set("primary-test-key");
  if (alias) {
    alias->set("alias-test-key");
  }
  require(wuwe::normalize_llm_client_config(id, cfg)->api_key == "primary-test-key",
    "primary environment credential did not win");
  if (alias_env) {
    primary.set("");
    require(wuwe::normalize_llm_client_config(id, cfg)->api_key == "alias-test-key",
      "provider alias credential was not resolved");
    primary.set("primary-test-key");
  }
  auto http = std::make_shared<capture_http_client>(body);
  Client client(cfg, http);
  require(client.complete(request).content == "ok", "new provider completion failed");
  require(http->requests.front().url == endpoint, "new provider endpoint incorrect");
  require(request_header_value(http->requests.front(), "Authorization") ==
            std::optional<std::string_view>("Bearer primary-test-key"),
    "provider credential not sent");
  auto payload = nlohmann::json::parse(http->requests.front().body);
  require(payload["model"] == "test-model", "configured model was replaced");

  cfg.api_key = "explicit-key";
  cfg.base_url = "https://private.example/prefix";
  cfg.chat_completions_path = "/custom/chat";
  request.model = "ep-user-selected-model";
  auto override_http = std::make_shared<capture_http_client>(body);
  Client overridden(cfg, override_http);
  (void)overridden.complete(request);
  payload = nlohmann::json::parse(override_http->requests.front().body);
  require(payload["model"] == "ep-user-selected-model", "request model/endpoint ID not preserved");
  require(override_http->requests.front().url == "https://private.example/prefix/custom/chat",
    "explicit endpoint override lost");
  require(request_header_value(override_http->requests.front(), "Authorization") ==
            std::optional<std::string_view>("Bearer explicit-key"),
    "explicit key did not win");

  cfg = { .load_api_key_from_environment = false, .model = "test-model", .max_retries = 0 };
  auto disabled_http = std::make_shared<capture_http_client>(body);
  Client disabled(cfg, disabled_http);
  require(disabled.complete(request).error_code == wuwe::agent::llm_error_code::missing_api_key &&
            disabled_http->requests.empty(),
    "disabled environment loading was ignored");

  cfg.api_key = "test-key";
  auto tool_http = std::make_shared<capture_http_client>(
    R"({"choices":[{"message":{"content":null,"tool_calls":[{"id":"call-1","type":"function","function":{"name":"lookup","arguments":"{\"q\":\"test\"}"}}]},"finish_reason":"tool_calls"}]})");
  Client tools(cfg, tool_http);
  request.tools = { { .name = "lookup",
    .description = "Look up a value",
    .parameters_json_schema = R"({"type":"object","properties":{"q":{"type":"string"}}})" } };
  const auto tool_result = tools.complete(request);
  require(!tool_result.error_code && tool_result.tool_calls.size() == 1 &&
            tool_result.tool_calls[0].name == "lookup",
    "new provider tool call failed");
  require(
    nlohmann::json::parse(tool_http->requests[0].body)["tools"][0]["function"]["name"] == "lookup",
    "tool schema not serialized");

  auto stream_http = std::make_shared<capture_http_client>(
    "data: {\"choices\":[{\"delta\":{\"content\":\"hello\"},\"finish_reason\":null}]}\n\n"
    "data: "
    "{\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":3,"
    "\"completion_tokens\":2,\"total_tokens\":5}}\n\n"
    "data: [DONE]\n\n");
  Client streaming(cfg, stream_http);
  std::string emitted;
  const auto stream_result = streaming.complete_stream(request,
    {
      .on_event =
        [&](const wuwe::llm_stream_event& event) {
          if (event.type == wuwe::llm_stream_event_type::content_delta) {
            emitted += event.content_delta;
          }
        },
    });
  require(!stream_result.error_code && stream_result.content == "hello" && emitted == "hello" &&
            stream_result.usage.total_tokens == 5,
    "new provider streaming/usage failed");
  require(
    stream_http->requests[0].url == endpoint, "streaming and completion use different endpoints");
}

void test_new_provider_presets() {
  verify_new_provider<wuwe::nvidia_llm_client>(
    "Nvidia", "https://integrate.api.nvidia.com/v1/chat/completions", "NVIDIA_API_KEY");
  verify_new_provider<wuwe::stepfun_llm_client>(
    "StepFun", "https://api.stepfun.com/v1/chat/completions", "STEPFUN_API_KEY");
  verify_new_provider<wuwe::mimo_llm_client>(
    "MiMo", "https://api.xiaomimimo.com/v1/chat/completions", "MIMO_API_KEY");
  verify_new_provider<wuwe::kimi_llm_client>(
    "Kimi", "https://api.moonshot.cn/v1/chat/completions", "MOONSHOT_API_KEY", "KIMI_API_KEY");
  verify_new_provider<wuwe::minimax_llm_client>(
    "MiniMax", "https://api.minimaxi.com/v1/chat/completions", "MINIMAX_API_KEY");
  verify_new_provider<wuwe::siliconflow_llm_client>(
    "SiliconFlow", "https://api.siliconflow.cn/v1/chat/completions", "SILICONFLOW_API_KEY");
  verify_new_provider<wuwe::doubao_llm_client>("Doubao",
    "https://ark.cn-beijing.volces.com/api/v3/chat/completions",
    "ARK_API_KEY",
    "DOUBAO_API_KEY");

  const auto cfg = wuwe::llm_client_config {
    .api_key = "test-key",
    .load_api_key_from_environment = false,
    .model = "test-model",
    .max_retries = 0,
  };
  wuwe::llm_request request;
  request.messages = {
    { .role = "assistant",
      .content = "",
      .reasoning_content = "retained provider state",
      .tool_calls = { { .id = "call-1", .name = "lookup", .arguments_json = "{}" } } },
    { .role = "tool", .content = "result", .tool_call_id = "call-1" },
  };
  request.thinking_mode = wuwe::llm_thinking_mode::disabled;
  const std::string body = R"({"choices":[{"message":{"content":"ok"},"finish_reason":"stop"}]})";
  auto kimi_http = std::make_shared<capture_http_client>(body);
  wuwe::kimi_llm_client kimi(cfg, kimi_http);
  require(!kimi.complete(request).error_code, "Kimi tool continuation failed");
  const auto kimi_payload = nlohmann::json::parse(kimi_http->requests[0].body);
  require(kimi_payload["messages"][0]["reasoning_content"] == "retained provider state" &&
            kimi_payload["thinking"]["type"] == "disabled",
    "Kimi thinking contract lost");

  auto silicon_http = std::make_shared<capture_http_client>(body);
  wuwe::siliconflow_llm_client silicon(cfg, silicon_http);
  (void)silicon.complete(request);
  const auto silicon_payload = nlohmann::json::parse(silicon_http->requests[0].body);
  require(silicon_payload["messages"][0]["reasoning_content"] == "retained provider state" &&
            !silicon_payload.contains("thinking"),
    "SiliconFlow received a different vendor's thinking control");

  auto minimax_http = std::make_shared<capture_http_client>(
    R"({"choices":[{"message":{"content":"<think>provider state</think>answer"},"finish_reason":"stop"}]})");
  wuwe::minimax_llm_client minimax(cfg, minimax_http);
  const auto minimax_response = minimax.complete(request);
  require(minimax_response.content == "<think>provider state</think>answer" &&
            minimax_response.reasoning_summary.empty(),
    "MiniMax native thinking content was rewritten");
  require(!minimax.capabilities().reasoning_summary && !minimax.capabilities().tool_choice &&
            !minimax.capabilities().json_schema_output,
    "MiniMax advertised unimplemented controls");
  request.tools = { { .name = "lookup", .parameters_json_schema = R"({"type":"object"})" } };
  request.tool_choice = wuwe::llm_tool_choice { .mode = wuwe::llm_tool_choice_mode::required };
  require(
    minimax.complete(request).error_code == wuwe::agent::llm_error_code::unsupported_capability &&
      minimax_http->requests.size() == 1,
    "unsupported MiniMax controls reached the network");
}

template<typename Client>
void verify_priority_provider_protocol(bool mimo_controls) {
  const auto cfg = wuwe::llm_client_config {
    .api_key = "test-key",
    .load_api_key_from_environment = false,
    .model = "vendor/model-id",
    .max_retries = 0,
  };
  const std::string response_body =
    R"({"choices":[{"message":{"content":"answer","reasoning_content":"new-state"},"finish_reason":"stop"}]})";
  auto http = std::make_shared<capture_http_client>(response_body);
  Client client(cfg, http);
  wuwe::llm_request request;
  request.messages = {
    { .role = "assistant",
      .content = "",
      .reasoning_content = "verbatim-history",
      .tool_calls = { { .id = "c1", .name = "lookup", .arguments_json = "{}" } } },
    { .role = "tool", .content = "found", .tool_call_id = "c1" },
  };
  request.tools = { { .name = "lookup", .parameters_json_schema = R"({"type":"object"})" } };
  request.max_output_tokens = 256;
  request.temperature = 0.8;
  const auto result = client.complete(request);
  require(
    !result.error_code && result.content == "answer" && result.reasoning_summary == "new-state",
    "priority provider did not parse separate reasoning");
  const auto payload = nlohmann::json::parse(http->requests[0].body);
  require(payload["messages"][0]["reasoning_content"] == "verbatim-history",
    "priority provider dropped tool-continuation reasoning");
  const auto* limit_key = mimo_controls ? "max_completion_tokens" : "max_tokens";
  const auto* other_limit_key = mimo_controls ? "max_tokens" : "max_completion_tokens";
  require(
    payload[limit_key] == 256 && !payload.contains(other_limit_key), "wrong output-limit field");
  require(payload["temperature"] == 0.8 && !payload.contains("thinking"),
    "preset silently changed sampling or default thinking behavior");

  for (const auto mode : { wuwe::llm_thinking_mode::enabled, wuwe::llm_thinking_mode::disabled }) {
    request.thinking_mode = mode;
    (void)client.complete(request);
    const auto explicit_payload = nlohmann::json::parse(http->requests.back().body);
    if (mimo_controls) {
      require(explicit_payload["thinking"]["type"] ==
                (mode == wuwe::llm_thinking_mode::enabled ? "enabled" : "disabled"),
        "MiMo thinking control not mapped");
    }
    else {
      require(
        !explicit_payload.contains("thinking"), "vendor-specific switch leaked to another preset");
    }
  }
  if (mimo_controls) {
    const auto sent = http->requests.size();
    request.tool_choice = wuwe::llm_tool_choice { .mode = wuwe::llm_tool_choice_mode::required };
    require(
      client.complete(request).error_code == wuwe::agent::llm_error_code::unsupported_capability &&
        http->requests.size() == sent,
      "MiMo accepted an unsupported tool-choice guarantee");
    request.tool_choice.reset();
  }

  auto stream_http = std::make_shared<capture_http_client>(
    "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"plan\"},\"finish_reason\":null}]}\n\n"
    "data: "
    "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"c2\",\"type\":\"function\","
    "\"function\":{\"name\":\"lookup\",\"arguments\":\"{}\"}}]},\"finish_reason\":null}]}\n\n"
    "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n"
    "data: [DONE]\n\n");
  Client streaming(cfg, stream_http);
  std::string reasoning;
  const auto streamed = streaming.complete_stream(request,
    {
      .on_reasoning_delta = [&](std::string_view delta) { reasoning += delta; },
    });
  require(!streamed.error_code && streamed.reasoning_summary == "plan" && reasoning == "plan" &&
            streamed.tool_calls.size() == 1 && streamed.tool_calls[0].name == "lookup" &&
            streamed.tool_calls[0].arguments_json == "{}",
    "priority provider reasoning/tool stream failed");
  const auto stream_payload = nlohmann::json::parse(stream_http->requests[0].body);
  require(stream_payload[limit_key] == 256 && !stream_payload.contains(other_limit_key),
    "streaming output-limit mapping differs from completion");
}

void test_priority_provider_protocols() {
  verify_priority_provider_protocol<wuwe::nvidia_llm_client>(false);
  verify_priority_provider_protocol<wuwe::stepfun_llm_client>(false);
  verify_priority_provider_protocol<wuwe::mimo_llm_client>(true);
}

void test_openai_compatible_provider_presets() {
  const std::string openai_body =
    R"({"choices":[{"message":{"content":"ok"},"finish_reason":"stop"}]})";
  wuwe::llm_request request;
  request.messages.push_back({ .role = "user", .content = "hello" });

  auto custom_path_http = std::make_shared<capture_http_client>(openai_body);
  wuwe::openai_compatible_llm_client custom_path_client(
    {
      .base_url = "https://gateway.example/api",
      .chat_completions_path = "custom/chat",
      .api_key = "",
      .require_api_key = false,
      .model = "compatible-test",
    },
    custom_path_http);
  (void)custom_path_client.complete(request);
  require(custom_path_http->requests.front().url == "https://gateway.example/api/custom/chat",
    "OpenAI-compatible client should honor a custom chat completions path");

  auto trailing_slash_http = std::make_shared<capture_http_client>(openai_body);
  wuwe::openai_compatible_llm_client trailing_slash_client(
    {
      .base_url = "https://gateway.example/api/",
      .chat_completions_path = "/custom/chat",
      .api_key = "",
      .require_api_key = false,
      .model = "compatible-test",
    },
    trailing_slash_http);
  (void)trailing_slash_client.complete(request);
  require(trailing_slash_http->requests.front().url == "https://gateway.example/api/custom/chat",
    "OpenAI-compatible client should avoid double slashes when joining URL paths");

  auto openai_http = std::make_shared<capture_http_client>(openai_body);
  wuwe::openai_llm_client openai(
    {
      .api_key = "",
      .require_api_key = false,
      .model = "gpt-test",
    },
    openai_http);
  require(openai.complete(request).content == "ok", "OpenAI preset should parse response");
  require(openai_http->requests.front().url == "https://api.openai.com/v1/chat/completions",
    "OpenAI preset should use the OpenAI base URL");

  auto deepseek_http = std::make_shared<capture_http_client>(openai_body);
  wuwe::deepseek_llm_client deepseek(
    {
      .api_key = "",
      .require_api_key = false,
      .model = "deepseek-test",
    },
    deepseek_http);
  (void)deepseek.complete(request);
  require(deepseek_http->requests.front().url == "https://api.deepseek.com/v1/chat/completions",
    "DeepSeek preset should use the DeepSeek base URL");

  auto dashscope_http = std::make_shared<capture_http_client>(openai_body);
  wuwe::dashscope_llm_client dashscope(
    {
      .api_key = "",
      .require_api_key = false,
      .model = "qwen-test",
    },
    dashscope_http);
  (void)dashscope.complete(request);
  require(dashscope_http->requests.front().url ==
            "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions",
    "DashScope preset should use the compatible-mode base URL");

  auto qwen_http = std::make_shared<capture_http_client>(openai_body);
  wuwe::qwen_llm_client qwen(
    {
      .api_key = "",
      .require_api_key = false,
      .model = "qwen-test",
    },
    qwen_http);
  (void)qwen.complete(request);
  require(qwen_http->requests.front().url ==
            "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions",
    "Qwen preset should use the DashScope compatible-mode base URL");

  auto zhipu_http = std::make_shared<capture_http_client>(openai_body);
  wuwe::zhipu_llm_client zhipu(
    {
      .api_key = "",
      .require_api_key = false,
      .model = "glm-test",
    },
    zhipu_http);
  (void)zhipu.complete(request);
  require(
    zhipu_http->requests.front().url == "https://open.bigmodel.cn/api/paas/v4/chat/completions",
    "Zhipu preset should use the BigModel v4 chat completions URL");

  auto zhipu_stream_http = std::make_shared<capture_http_client>(
    "data: {\"choices\":[{\"delta\":{\"content\":\"hi\"},\"finish_reason\":\"stop\"}]}\n\n"
    "data: [DONE]\n\n");
  wuwe::zhipu_llm_client zhipu_stream(
    {
      .api_key = "",
      .require_api_key = false,
      .model = "glm-test",
    },
    zhipu_stream_http);
  std::vector<wuwe::llm_stream_event> events;
  wuwe::llm_stream_callbacks callbacks;
  callbacks.on_event = [&](const wuwe::llm_stream_event& event) { events.push_back(event); };
  const auto zhipu_stream_response = zhipu_stream.complete_stream(request, callbacks);
  require(!zhipu_stream_response.error_code,
    "Zhipu preset should reuse OpenAI-compatible streaming parsing");
  require(zhipu_stream_response.content == "hi",
    "Zhipu preset should aggregate OpenAI-compatible streaming content");
  require(zhipu_stream_http->requests.front().url ==
            "https://open.bigmodel.cn/api/paas/v4/chat/completions",
    "Zhipu streaming should use the BigModel v4 chat completions URL");
}

void test_native_provider_clients_parse_text_and_tools() {
  wuwe::llm_request request;
  request.messages.push_back({ .role = "user", .content = "hello" });

  auto anthropic_http = std::make_shared<capture_http_client>(
    R"({"content":[{"type":"thinking","thinking":"inspect","signature":"sig-a"},{"type":"text","text":"hi"},{"type":"tool_use","id":"t1","name":"lookup","input":{"q":"x"}}],"stop_reason":"tool_use","usage":{"input_tokens":2,"cache_read_input_tokens":1,"cache_creation_input_tokens":2,"output_tokens":3}})");
  wuwe::anthropic_llm_client anthropic(
    {
      .api_key = "",
      .require_api_key = false,
      .model = "claude-test",
    },
    anthropic_http);
  const auto anthropic_response = anthropic.complete(request);
  require(anthropic_http->requests.front().url == "https://api.anthropic.com/v1/messages",
    "Anthropic client should use the Messages API URL");
  require(has_request_header(anthropic_http->requests.front(), "anthropic-version"),
    "Anthropic client should send anthropic-version");
  require(anthropic_response.content == "hi", "Anthropic client should parse text blocks");
  require(anthropic_response.reasoning_summary == "inspect",
    "Anthropic client should parse thinking blocks separately");
  require(anthropic_response.reasoning_metadata.at("signature") == "sig-a",
    "Anthropic client should preserve thinking signatures as metadata");
  require(
    anthropic_response.tool_calls.size() == 1, "Anthropic client should parse tool_use blocks");
  require(anthropic_response.usage.prompt_tokens == 5 &&
            anthropic_response.usage.cached_prompt_tokens == 1 &&
            anthropic_response.usage.total_tokens == 8,
    "Anthropic usage should include cache read and creation input tokens");

  auto gemini_http = std::make_shared<capture_http_client>(
    R"({"candidates":[{"content":{"parts":[{"text":"inspect","thought":true,"thoughtSignature":"sig-g"},{"text":"hi"},{"functionCall":{"name":"lookup","args":{"q":"x"}}}]},"finishReason":"STOP"}],"usageMetadata":{"promptTokenCount":2,"cachedContentTokenCount":1,"candidatesTokenCount":3,"thoughtsTokenCount":1,"totalTokenCount":6}})");
  wuwe::gemini_llm_client gemini(
    {
      .api_key = "",
      .require_api_key = false,
      .model = "gemini-test",
    },
    gemini_http);
  const auto gemini_response = gemini.complete(request);
  require(gemini_http->requests.front().url ==
            "https://generativelanguage.googleapis.com/v1beta/models/gemini-test:generateContent",
    "Gemini client should use the generateContent URL");
  require(has_request_header(gemini_http->requests.front(), "x-goog-api-key") == false,
    "Gemini client should omit empty API key header");
  require(gemini_response.content == "hi", "Gemini client should parse text parts");
  require(gemini_response.reasoning_summary == "inspect",
    "Gemini client should parse thought parts separately");
  require(gemini_response.reasoning_metadata.at("thought_signature") == "sig-g",
    "Gemini client should preserve thought signatures as metadata");
  require(gemini_response.tool_calls.size() == 1, "Gemini client should parse functionCall parts");
  require(gemini_response.usage.cached_prompt_tokens == 1 &&
            gemini_response.usage.reasoning_tokens == 1 &&
            gemini_response.usage.completion_tokens == 4 && gemini_response.usage.total_tokens == 6,
    "Gemini usage should expose cached and thought tokens");

  auto ollama_http = std::make_shared<capture_http_client>(
    R"({"message":{"role":"assistant","thinking":"inspect","content":"hi","tool_calls":[{"function":{"name":"lookup","arguments":{"q":"x"}}}]},"done_reason":"stop","prompt_eval_count":2,"eval_count":3})");
  wuwe::ollama_llm_client ollama(
    {
      .model = "llama-test",
    },
    ollama_http);
  const auto ollama_response = ollama.complete(request);
  require(ollama_http->requests.front().url == "http://localhost:11434/api/chat",
    "Ollama client should use the local chat API URL");
  require(ollama_response.content == "hi", "Ollama client should parse message content");
  require(ollama_response.reasoning_summary == "inspect",
    "Ollama client should parse thinking content separately");
  require(ollama_response.tool_calls.size() == 1, "Ollama client should parse tool_calls");
}

void test_execution_context_trace_reaches_provider_http_requests() {
  wuwe::llm_request request;
  request.messages.push_back({ .role = "user", .content = "hello" });
  request.execution_context = wuwe::agent::core::agent_execution_context {
    .run_id = "run-trace",
    .trace_id = "trace-provider",
  };

  auto openai_http = std::make_shared<capture_http_client>(
    R"({"choices":[{"message":{"content":"ok"},"finish_reason":"stop"}]})");
  wuwe::openai_llm_client openai(
    {
      .api_key = "",
      .require_api_key = false,
      .model = "gpt-test",
    },
    openai_http);
  (void)openai.complete(request);

  auto anthropic_http = std::make_shared<capture_http_client>(
    R"({"content":[{"type":"text","text":"ok"}],"stop_reason":"end_turn"})");
  wuwe::anthropic_llm_client anthropic(
    {
      .api_key = "",
      .require_api_key = false,
      .model = "claude-test",
    },
    anthropic_http);
  (void)anthropic.complete(request);

  auto gemini_http = std::make_shared<capture_http_client>(
    R"({"candidates":[{"content":{"parts":[{"text":"ok"}]},"finishReason":"STOP"}]})");
  wuwe::gemini_llm_client gemini(
    {
      .api_key = "",
      .require_api_key = false,
      .model = "gemini-test",
    },
    gemini_http);
  (void)gemini.complete(request);

  auto ollama_http = std::make_shared<capture_http_client>(
    R"({"message":{"role":"assistant","content":"ok"},"done_reason":"stop"})");
  wuwe::ollama_llm_client ollama({ .model = "llama-test" }, ollama_http);
  (void)ollama.complete(request);

  for (const auto* captured : {
         openai_http.get(),
         anthropic_http.get(),
         gemini_http.get(),
         ollama_http.get(),
       }) {
    require(
      captured->requests.size() == 1 && captured->requests.front().trace_id == "trace-provider",
      "provider HTTP requests should preserve the agent execution trace id");
  }
}

void test_reasoning_language_contract_is_mapped_to_provider_payloads() {
  wuwe::llm_request request;
  request.language = {
    .response_language = "zh-CN",
    .reasoning_language = "zh-CN",
    .locale = "zh-CN",
  };
  request.max_output_tokens = 321;
  request.messages.push_back({ .role = "user", .content = "分析一下当前应用" });

  auto openai_http = std::make_shared<capture_http_client>(
    R"({"choices":[{"message":{"reasoning_content":"The user wants analysis.","content":"好的"},"finish_reason":"stop"}],"usage":{"prompt_tokens":10,"completion_tokens":6,"total_tokens":16,"prompt_tokens_details":{"cached_tokens":4},"completion_tokens_details":{"reasoning_tokens":2}}})");
  wuwe::openai_compatible_llm_client openai(
    {
      .base_url = "https://compatible.example",
      .api_key = "",
      .require_api_key = false,
      .model = "test-model",
    },
    openai_http);
  const auto openai_response = openai.complete(request);
  const auto openai_body = nlohmann::json::parse(openai_http->requests.front().body);
  require(openai_body["messages"].front().value("role", std::string {}) == "system",
    "OpenAI-compatible language contract should be the first system message");
  require(openai_body["messages"].front().value("content", std::string {}).find("zh-CN") !=
            std::string::npos,
    "OpenAI-compatible language contract should include requested language");
  require(openai_body.value("max_tokens", 0) == 321,
    "OpenAI-compatible requests should carry the output token limit");
  require(openai_response.reasoning_metadata.at("requested_language") == "zh-CN",
    "reasoning metadata should preserve requested reasoning language");
  require(openai_response.reasoning_metadata.at("detected_language") == "en",
    "reasoning metadata should detect obvious English reasoning");
  require(openai_response.reasoning_metadata.at("language_mismatch") == "true",
    "reasoning metadata should mark requested/detected language mismatch");
  require(openai_response.reasoning_metadata.at("language_control") == "prompt_contract",
    "reasoning metadata should identify prompt-contract language control");
  require(
    openai_response.usage.cached_prompt_tokens == 4 && openai_response.usage.reasoning_tokens == 2,
    "OpenAI-compatible usage should expose cached and reasoning token details");

  auto anthropic_http = std::make_shared<capture_http_client>(
    R"({"content":[{"type":"text","text":"好的"}],"stop_reason":"end_turn"})");
  wuwe::anthropic_llm_client anthropic(
    {
      .api_key = "",
      .require_api_key = false,
      .model = "claude-test",
    },
    anthropic_http);
  (void)anthropic.complete(request);
  const auto anthropic_body = nlohmann::json::parse(anthropic_http->requests.front().body);
  require(anthropic_body.value("system", std::string {}).find("zh-CN") != std::string::npos,
    "Anthropic language contract should be carried in system");
  require(anthropic_body.value("max_tokens", 0) == 321,
    "Anthropic requests should carry the output token limit");

  auto gemini_http = std::make_shared<capture_http_client>(
    R"({"candidates":[{"content":{"parts":[{"text":"好的"}]},"finishReason":"STOP"}]})");
  wuwe::gemini_llm_client gemini(
    {
      .api_key = "",
      .require_api_key = false,
      .model = "gemini-test",
    },
    gemini_http);
  (void)gemini.complete(request);
  const auto gemini_body = nlohmann::json::parse(gemini_http->requests.front().body);
  require(
    gemini_body["systemInstruction"]["parts"].front().value("text", std::string {}).find("zh-CN") !=
      std::string::npos,
    "Gemini language contract should be carried in systemInstruction");
  require(gemini_body["generationConfig"].value("maxOutputTokens", 0) == 321,
    "Gemini requests should carry the output token limit");

  auto ollama_http = std::make_shared<capture_http_client>(
    R"({"message":{"role":"assistant","content":"好的"},"done_reason":"stop"})");
  wuwe::ollama_llm_client ollama(
    {
      .model = "llama-test",
    },
    ollama_http);
  (void)ollama.complete(request);
  const auto ollama_body = nlohmann::json::parse(ollama_http->requests.front().body);
  require(ollama_body["messages"].front().value("role", std::string {}) == "system",
    "Ollama language contract should be the first system message");
  require(ollama_body["messages"].front().value("content", std::string {}).find("zh-CN") !=
            std::string::npos,
    "Ollama language contract should include requested language");
  require(ollama_body["options"].value("num_predict", 0) == 321,
    "Ollama requests should carry the output token limit");
}

void test_openai_compatible_replays_reasoning_content() {
  auto http = std::make_shared<capture_http_client>(
    R"({"choices":[{"message":{"content":"done"},"finish_reason":"stop"}]})");
  wuwe::deepseek_llm_client client(
    {
      .api_key = "",
      .require_api_key = false,
      .model = "deepseek-test",
    },
    http);
  wuwe::llm_request request;
  request.messages.push_back({ .role = "user", .content = "inspect" });
  request.messages.push_back({
    .role = "assistant",
    .content = "",
    .reasoning_content = "verbatim provider reasoning",
    .tool_calls = {
      { .id = "call-1", .name = "lookup", .arguments_json = "{}" },
    },
  });
  request.messages.push_back({
    .role = "tool",
    .content = "result",
    .tool_call_id = "call-1",
  });

  (void)client.complete(request);
  const auto payload = nlohmann::json::parse(http->requests.front().body);
  const auto& assistant = payload.at("messages").at(1);
  require(assistant.value("reasoning_content", std::string {}) ==
      "verbatim provider reasoning",
    "OpenAI-compatible clients must replay provider reasoning state verbatim");
}

void test_deepseek_normalizes_validated_dsml_tool_calls() {
  const std::string dsml =
    "I will inspect the device.\n<｜DSML｜tool_calls>"
    "<｜DSML｜invoke name=“lookup”>"
    "<｜DSML｜parameter name=“query” string=“true”>model</｜DSML｜parameter>"
    "<｜DSML｜parameter name=“limit” string=“false”>3</｜DSML｜parameter>"
    "</｜DSML｜invoke></｜DSML｜tool_calls>";
  const auto body = nlohmann::json {
    { "choices",
      nlohmann::json::array({ { { "message", { { "content", dsml } } },
        { "finish_reason", "stop" } } }) },
  }.dump();
  auto http = std::make_shared<capture_http_client>(body);
  wuwe::deepseek_llm_client client(
    { .api_key = "", .require_api_key = false, .model = "deepseek-test" }, http);
  wuwe::llm_request request;
  request.messages.push_back({ .role = "user", .content = "inspect" });
  request.tools.push_back({
    .name = "lookup",
    .description = "Lookup device metadata",
    .parameters_json_schema = R"({"type":"object"})",
  });

  const auto response = client.complete(request);
  require(!response.error_code, "valid DSML should normalize successfully");
  require(response.content == "I will inspect the device.",
    "ordinary assistant text preceding DSML must remain visible response content");
  require(response.reasoning_summary.empty(),
    "ordinary assistant text preceding DSML must not be reclassified as reasoning");
  require(response.tool_calls.size() == 1 && response.tool_calls.front().name == "lookup",
    "DSML should produce one validated tool call");
  const auto arguments = nlohmann::json::parse(response.tool_calls.front().arguments_json);
  require(arguments.at("query") == "model" && arguments.at("limit") == 3,
    "DSML string and JSON-typed parameters should retain their types");
  require(response.metadata.count("wuwe_tool_protocol_normalized") == 1,
    "normalized provider protocol should be observable in metadata");
}

void test_deepseek_rejects_unregistered_or_malformed_dsml() {
  const auto make_body = [](const std::string& content) {
    return nlohmann::json {
      { "choices", nlohmann::json::array(
          { { { "message", { { "content", content } } } } }) },
    }.dump();
  };
  auto http = std::make_shared<capture_http_client>(std::vector<wuwe::http_response> {
    { .body = make_body(
        R"(<|DSML|tool_calls><|DSML|invoke name="unknown"></|DSML|invoke></|DSML|tool_calls>)") },
    { .body = make_body(
        R"(<|DSML|tool_calls><|DSML|invoke name="lookup"><|DSML|parameter name="limit" string="false">not-json</|DSML|parameter></|DSML|invoke></|DSML|tool_calls>)") },
  });
  wuwe::deepseek_llm_client client(
    { .api_key = "", .require_api_key = false, .model = "deepseek-test" }, http);
  wuwe::llm_request request;
  request.messages.push_back({ .role = "user", .content = "inspect" });
  request.tools.push_back({ .name = "lookup" });

  const auto unknown = client.complete(request);
  require(unknown.tool_calls.empty() && !unknown.content.empty(),
    "unregistered DSML tools must never become executable calls");
  const auto malformed = client.complete(request);
  require(malformed.tool_calls.empty() && !malformed.content.empty(),
    "malformed typed DSML parameters must never become executable calls");
}

void test_deepseek_negotiates_unsupported_explicit_tool_choice() {
  auto http = std::make_shared<capture_http_client>(std::vector<wuwe::http_response> {
    { .body = R"({"error":{"message":"Thinking mode does not support this tool_choice"}})" },
    { .body = R"({"choices":[{"message":{"content":"ok"},"finish_reason":"stop"}]})" },
    { .body = R"({"choices":[{"message":{"content":"again"},"finish_reason":"stop"}]})" },
  });
  wuwe::deepseek_llm_client client(
    { .api_key = "", .require_api_key = false, .model = "deepseek-test" }, http);
  wuwe::llm_request request;
  request.messages.push_back({ .role = "user", .content = "inspect" });
  request.tools.push_back({ .name = "lookup" });
  request.tool_choice = wuwe::llm_tool_choice {
    .mode = wuwe::llm_tool_choice_mode::required,
  };

  const auto first = client.complete(request);
  require(!first.error_code && first.content == "ok" && http->requests.size() == 2,
    "DeepSeek should retry once after rejecting explicit tool_choice");
  require(nlohmann::json::parse(http->requests.front().body).contains("tool_choice"),
    "the initial request should retain the caller's explicit tool choice");
  require(!nlohmann::json::parse(http->requests.at(1).body).contains("tool_choice"),
    "the negotiated retry should omit unsupported tool_choice");
  const auto second = client.complete(request);
  require(!second.error_code && second.content == "again" && http->requests.size() == 3,
    "the negotiated provider capability should remain sticky for the client");
  require(!nlohmann::json::parse(http->requests.back().body).contains("tool_choice"),
    "subsequent requests should use the learned provider capability");
}

void test_deepseek_maps_request_scoped_thinking_control() {
  auto http = std::make_shared<capture_http_client>(
    R"({"choices":[{"message":{"content":"{\"operation\":\"answer\"}"},"finish_reason":"stop"}]})");
  wuwe::deepseek_llm_client client(
    { .api_key = "", .require_api_key = false, .model = "deepseek-test" }, http);
  wuwe::llm_request request;
  request.messages.push_back({ .role = "user", .content = "classify" });
  request.thinking_mode = wuwe::llm_thinking_mode::disabled;

  const auto response = client.complete(request);
  require(!response.error_code && http->requests.size() == 1,
    "DeepSeek lightweight requests should complete normally");
  const auto payload = nlohmann::json::parse(http->requests.front().body);
  require(payload.contains("thinking") &&
            payload.at("thinking").value("type", std::string {}) == "disabled",
    "DeepSeek should receive request-scoped thinking.type=disabled");
}

void test_deepseek_streams_while_filtering_text_tool_protocol() {
  const std::string body =
    "data: {\"choices\":[{\"delta\":{\"content\":\"I will inspect the target.\\n<|DSML|tool_\"},"
    "\"finish_reason\":null}]}\n\n"
    "data: {\"choices\":[{\"delta\":{\"content\":\"calls><|DSML|invoke "
    "name=\\\"lookup\\\"></|DSML|invoke></|DSML|tool_calls>\"},"
    "\"finish_reason\":\"tool_calls\"}]}\n\n"
    "data: [DONE]\n\n";
  auto http = std::make_shared<capture_http_client>(body);
  wuwe::deepseek_llm_client client(
    { .api_key = "", .require_api_key = false, .model = "deepseek-test" }, http);
  wuwe::llm_request request;
  request.messages.push_back({ .role = "user", .content = "inspect" });
  request.tools.push_back({ .name = "lookup" });
  std::string visible_content;
  std::vector<wuwe::llm_tool_call> completed_calls;
  wuwe::llm_stream_callbacks callbacks;
  callbacks.on_event = [&](const wuwe::llm_stream_event& event) {
    visible_content += event.content_delta;
    if (event.type == wuwe::llm_stream_event_type::tool_call_done && event.tool_call) {
      completed_calls.push_back(*event.tool_call);
    }
  };

  const auto response = client.complete_stream(request, callbacks);
  require(!response.error_code && response.tool_calls.size() == 1,
    "buffered DeepSeek completion should retain the normalized tool call");
  require(visible_content == "I will inspect the target.\n" &&
            visible_content.find("DSML") == std::string::npos,
    "ordinary assistant commentary should stream while provider protocol remains hidden");
  require(response.content == "I will inspect the target.",
    "streamed commentary must remain response content after DSML normalization");
  require(completed_calls.size() == 1 && completed_calls.front().name == "lookup",
    "buffered protocol should emit a structured tool_call_done event");
  const auto payload = nlohmann::json::parse(http->requests.front().body);
  require(payload.value("stream", false),
    "text-protocol filtering must retain real provider streaming");
}

void test_deepseek_stream_filter_restores_invalid_protocol_text() {
  const std::string body =
    "data: {\"choices\":[{\"delta\":{\"content\":\"Literal <|DSML|tool_\"},"
    "\"finish_reason\":null}]}\n\n"
    "data: {\"choices\":[{\"delta\":{\"content\":\"calls is documentation, not a call.\"},"
    "\"finish_reason\":\"stop\"}]}\n\n"
    "data: [DONE]\n\n";
  auto http = std::make_shared<capture_http_client>(body);
  wuwe::deepseek_llm_client client(
    { .api_key = "", .require_api_key = false, .model = "deepseek-test" }, http);
  wuwe::llm_request request;
  request.messages.push_back({ .role = "user", .content = "explain the protocol" });
  request.tools.push_back({ .name = "lookup" });
  std::string visible_content;
  wuwe::llm_stream_callbacks callbacks;
  callbacks.on_event = [&](const wuwe::llm_stream_event& event) {
    visible_content += event.content_delta;
  };

  const auto response = client.complete_stream(request, callbacks);
  require(!response.error_code && response.tool_calls.empty(),
    "invalid DSML-like prose must remain an ordinary successful response");
  require(visible_content == response.content &&
            response.content == "Literal <|DSML|tool_calls is documentation, not a call.",
    "a false-positive protocol prefix must not truncate streamed or final content");
}

void test_openai_compatible_rejects_reasoning_only_terminal_stream() {
  const std::string body =
    "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"working\"},"
    "\"finish_reason\":null}]}\n\n"
    "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
    "data: [DONE]\n\n";
  auto http = std::make_shared<capture_http_client>(body);
  wuwe::deepseek_llm_client client(
    { .api_key = "", .require_api_key = false, .model = "deepseek-test" }, http);
  wuwe::llm_request request;
  request.messages.push_back({ .role = "user", .content = "answer" });

  const auto response = client.complete_stream(request, {});
  require(response.error_code == wuwe::agent::llm_error_code::invalid_response,
    "reasoning-only terminal streams must not be reported as successful answers");
  require(response.stop_reason == "empty_terminal_response",
    "empty terminal responses should expose a stable diagnostic stop reason");
  require(response.metadata.at("reasoning_bytes") == "7",
    "empty-response diagnostics should preserve the observed reasoning size");
}

void test_advanced_generation_capabilities_are_mapped_or_rejected() {
  const nlohmann::json schema {
    { "type", "object" },
    { "properties", { { "answer", { { "type", "string" } } } } },
    { "required", nlohmann::json::array({ "answer" }) },
    { "additionalProperties", false },
  };
  wuwe::llm_request request;
  request.messages.push_back({ .role = "user", .content = "answer" });
  request.stop_sequences = { "END" };
  request.seed = 42;
  request.json_schema_output = {
    .name = "answer_schema",
    .schema = schema,
  };

  auto openai_http = std::make_shared<capture_http_client>(
    R"({"choices":[{"message":{"content":"{\"answer\":\"ok\"}"},"finish_reason":"stop"}]})");
  wuwe::openai_compatible_llm_client openai(
    {
      .base_url = "https://compatible.example",
      .api_key = "",
      .require_api_key = false,
      .model = "test-model",
      .capabilities_override =
        wuwe::llm_provider_capabilities {
          .streaming = true,
          .stop_sequences = true,
          .deterministic_seed = true,
          .json_schema_output = true,
        },
    },
    openai_http);
  require(!openai.complete(request).error_code,
    "capability-enabled OpenAI-compatible request should succeed");
  const auto openai_body = nlohmann::json::parse(openai_http->requests.front().body);
  require(openai_body.at("stop").front() == "END" && openai_body.at("seed") == 42 &&
            openai_body.at("response_format").at("type") == "json_schema" &&
            openai_body.at("response_format").at("json_schema").at("schema") == schema,
    "OpenAI-compatible payload should preserve advanced generation controls");

  auto gemini_http = std::make_shared<capture_http_client>(
    R"({"candidates":[{"content":{"parts":[{"text":"{\"answer\":\"ok\"}"}]},"finishReason":"STOP"}]})");
  wuwe::gemini_llm_client gemini(
    {
      .api_key = "",
      .require_api_key = false,
      .model = "gemini-test",
    },
    gemini_http);
  require(
    !gemini.complete(request).error_code, "Gemini advanced generation request should succeed");
  const auto gemini_config =
    nlohmann::json::parse(gemini_http->requests.front().body).at("generationConfig");
  require(gemini_config.at("stopSequences").front() == "END" && gemini_config.at("seed") == 42 &&
            gemini_config.at("responseMimeType") == "application/json" &&
            gemini_config.at("responseSchema") == schema,
    "Gemini payload should preserve advanced generation controls");

  auto ollama_http = std::make_shared<capture_http_client>(
    R"({"message":{"role":"assistant","content":"{\"answer\":\"ok\"}"},"done_reason":"stop"})");
  wuwe::ollama_llm_client ollama({ .model = "llama-test" }, ollama_http);
  require(
    !ollama.complete(request).error_code, "Ollama advanced generation request should succeed");
  const auto ollama_body = nlohmann::json::parse(ollama_http->requests.front().body);
  require(ollama_body.at("options").at("stop").front() == "END" &&
            ollama_body.at("options").at("seed") == 42 && ollama_body.at("format") == schema,
    "Ollama payload should preserve advanced generation controls");

  auto anthropic_http = std::make_shared<capture_http_client>(
    R"({"content":[{"type":"text","text":"unused"}],"stop_reason":"end_turn"})");
  wuwe::anthropic_llm_client anthropic(
    {
      .api_key = "",
      .require_api_key = false,
      .model = "claude-test",
    },
    anthropic_http);
  const auto rejected = anthropic.complete(request);
  require(rejected.error_code == wuwe::agent::llm_error_code::unsupported_capability &&
            rejected.metadata.at("unsupported_capability") == "deterministic_seed" &&
            anthropic_http->requests.empty(),
    "provider should reject unsupported controls before network dispatch");

  request.seed.reset();
  request.json_schema_output.reset();
  require(
    !anthropic.complete(request).error_code, "Anthropic should accept supported stop sequences");
  const auto anthropic_body = nlohmann::json::parse(anthropic_http->requests.front().body);
  require(anthropic_body.at("stop_sequences").front() == "END",
    "Anthropic payload should preserve supported stop sequences");

  request.cache_mode = wuwe::llm_cache_mode::enabled;
  const auto cache_rejected = anthropic.complete(request);
  require(cache_rejected.error_code == wuwe::agent::llm_error_code::unsupported_capability &&
            cache_rejected.metadata.at("unsupported_capability") == "explicit_cache_control",
    "explicit cache requests should fail instead of being silently ignored");

  request.cache_mode = wuwe::llm_cache_mode::provider_default;
  request.response_format = "json_object";
  const auto requests_before_format = anthropic_http->requests.size();
  const auto format_rejected = anthropic.complete(request);
  require(format_rejected.error_code == wuwe::agent::llm_error_code::unsupported_capability &&
            format_rejected.metadata.at("unsupported_capability") == "json_response_format" &&
            anthropic_http->requests.size() == requests_before_format,
    "legacy JSON response format should participate in capability validation");

  request.response_format.reset();
  request.stop_sequences.clear();
  request.tools = {
    { .name = "lookup", .parameters_json_schema = R"({"type":"object"})" },
  };
  request.tool_choice = wuwe::llm_tool_choice {
    .mode = wuwe::llm_tool_choice_mode::named,
    .name = "missing",
  };
  const auto invalid_choice = wuwe::agent::llm::validate_llm_request(request,
    {
      .tools = true,
      .tool_choice = true,
    });
  require(
    !invalid_choice && invalid_choice.error_code == wuwe::agent::llm_error_code::invalid_request,
    "named tool choice should reference a declared tool");

  request.tool_choice->mode = static_cast<wuwe::llm_tool_choice_mode>(99);
  const auto invalid_mode = wuwe::agent::llm::validate_llm_request(request,
    {
      .tools = true,
      .tool_choice = true,
    });
  require(!invalid_mode && invalid_mode.error_code == wuwe::agent::llm_error_code::invalid_request,
    "out-of-range tool choice modes should be rejected before provider mapping");

  request.tool_choice->mode = wuwe::llm_tool_choice_mode::named;
  request.tool_choice->name = "lookup";
  const auto tools_rejected = openai.complete(request);
  require(tools_rejected.error_code == wuwe::agent::llm_error_code::unsupported_capability &&
            tools_rejected.metadata.at("unsupported_capability") == "tools" &&
            openai_http->requests.size() == 1,
    "tool requests should fail before dispatch when the adapter disables tools");
}

void test_native_provider_streaming_success_and_incomplete_streams() {
  wuwe::llm_request request;
  request.messages.push_back({ .role = "user", .content = "hello" });

  std::vector<wuwe::llm_stream_event> events;
  wuwe::llm_stream_callbacks callbacks;
  callbacks.on_event = [&](const wuwe::llm_stream_event& event) { events.push_back(event); };

  auto anthropic_http = std::make_shared<capture_http_client>(
    "event: message_start\n"
    "data: {\"type\":\"message_start\",\"message\":{\"usage\":{\"input_tokens\":2}}}\n\n"
    "event: content_block_delta\n"
    "data: "
    "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"thinking_delta\","
    "\"thinking\":\"inspect\"}}\n\n"
    "event: content_block_delta\n"
    "data: "
    "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"signature_delta\","
    "\"signature\":\"sig-a\"}}\n\n"
    "event: content_block_delta\n"
    "data: "
    "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":"
    "\"hi\"}}\n\n"
    "event: message_delta\n"
    "data: "
    "{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},\"usage\":{\"output_"
    "tokens\":3}}\n\n"
    "event: message_stop\n"
    "data: {\"type\":\"message_stop\"}\n\n");
  wuwe::anthropic_llm_client anthropic(
    {
      .api_key = "",
      .require_api_key = false,
      .model = "claude-test",
    },
    anthropic_http);
  auto anthropic_response = anthropic.complete_stream(request, callbacks);
  require(!anthropic_response.error_code, "Anthropic stream should succeed");
  require(anthropic_response.content == "hi", "Anthropic stream should aggregate text");
  require(anthropic_response.reasoning_summary == "inspect",
    "Anthropic stream should aggregate thinking deltas separately");
  require(anthropic_response.reasoning_metadata.at("signature") == "sig-a",
    "Anthropic stream should preserve thinking signature metadata");
  require(anthropic_response.usage.total_tokens == 5, "Anthropic stream should parse usage");

  auto anthropic_incomplete_http =
    std::make_shared<capture_http_client>("event: content_block_delta\n"
                                          "data: "
                                          "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":"
                                          "{\"type\":\"text_delta\",\"text\":\"partial\"}}\n\n");
  wuwe::anthropic_llm_client anthropic_incomplete(
    {
      .api_key = "",
      .require_api_key = false,
      .model = "claude-test",
    },
    anthropic_incomplete_http);
  auto anthropic_incomplete_response = anthropic_incomplete.complete_stream(request, callbacks);
  require(anthropic_incomplete_response.error_code == wuwe::agent::llm_error_code::invalid_response,
    "Anthropic incomplete stream should fail");

  auto gemini_http = std::make_shared<capture_http_client>(
    "data: "
    "{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"inspect\",\"thought\":true,"
    "\"thoughtSignature\":\"sig-g\"},{\"text\":\"hi\"}]},\"finishReason\":\"STOP\"}],"
    "\"usageMetadata\":{\"promptTokenCount\":2,\"candidatesTokenCount\":3,\"totalTokenCount\":5}}"
    "\n\n");
  wuwe::gemini_llm_client gemini(
    {
      .api_key = "",
      .require_api_key = false,
      .model = "gemini-test",
    },
    gemini_http);
  auto gemini_response = gemini.complete_stream(request, callbacks);
  require(!gemini_response.error_code, "Gemini stream should succeed");
  require(gemini_response.content == "hi", "Gemini stream should aggregate text");
  require(gemini_response.reasoning_summary == "inspect",
    "Gemini stream should aggregate thought parts separately");
  require(gemini_response.reasoning_metadata.at("thought_signature") == "sig-g",
    "Gemini stream should preserve thought signature metadata");
  require(gemini_response.usage.total_tokens == 5, "Gemini stream should parse usage");

  auto gemini_incomplete_http = std::make_shared<capture_http_client>(
    "data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"partial\"}]}}]}\n\n");
  wuwe::gemini_llm_client gemini_incomplete(
    {
      .api_key = "",
      .require_api_key = false,
      .model = "gemini-test",
    },
    gemini_incomplete_http);
  auto gemini_incomplete_response = gemini_incomplete.complete_stream(request, callbacks);
  require(gemini_incomplete_response.error_code == wuwe::agent::llm_error_code::invalid_response,
    "Gemini incomplete stream should fail");

  auto ollama_http = std::make_shared<capture_http_client>(
    "{\"message\":{\"role\":\"assistant\",\"thinking\":\"inspect\",\"content\":\"hi\"},\"done\":"
    "false}\n"
    "{\"done\":true,\"done_reason\":\"stop\",\"prompt_eval_count\":2,\"eval_count\":3}\n");
  wuwe::ollama_llm_client ollama(
    {
      .model = "llama-test",
    },
    ollama_http);
  auto ollama_response = ollama.complete_stream(request, callbacks);
  require(!ollama_response.error_code, "Ollama stream should succeed");
  require(ollama_response.content == "hi", "Ollama stream should aggregate text");
  require(ollama_response.reasoning_summary == "inspect",
    "Ollama stream should aggregate thinking separately");
  require(ollama_response.usage.total_tokens == 5, "Ollama stream should parse usage");

  int reasoning_delta_events = 0;
  int reasoning_done_events = 0;
  for (const auto& event : events) {
    if (event.type == wuwe::llm_stream_event_type::reasoning_delta) {
      ++reasoning_delta_events;
      require(
        event.content_delta.empty(), "provider reasoning events should not carry content deltas");
    }
    if (event.type == wuwe::llm_stream_event_type::reasoning_done) {
      ++reasoning_done_events;
    }
  }
  require(reasoning_delta_events == 3,
    "native provider streams should emit reasoning deltas when supplied");
  require(reasoning_done_events == 3,
    "native provider streams should emit reasoning completion when supplied");

  auto ollama_incomplete_http = std::make_shared<capture_http_client>(
    "{\"message\":{\"role\":\"assistant\",\"content\":\"partial\"},\"done\":false}\n");
  wuwe::ollama_llm_client ollama_incomplete(
    {
      .model = "llama-test",
    },
    ollama_incomplete_http);
  auto ollama_incomplete_response = ollama_incomplete.complete_stream(request, callbacks);
  require(ollama_incomplete_response.error_code == wuwe::agent::llm_error_code::invalid_response,
    "Ollama incomplete stream should fail");
}

void require_stream_timeout_options(const wuwe::http_request& request) {
  require(
    request.timeouts.total_ms == 9000, "streaming total timeout should map to HTTP total timeout");
  require(request.timeouts.connect_ms == 1000,
    "streaming connect timeout should map to HTTP connect timeout");
  require(
    request.timeouts.read_ms == 3000, "streaming idle timeout should map to HTTP read timeout");
}

void test_native_provider_streaming_uses_stage_timeout_options() {
  wuwe::llm_request request;
  request.messages.push_back({ .role = "user", .content = "hello" });

  const wuwe::llm_stream_timeout_options stream_timeouts {
    .total_ms = 9000,
    .connect_ms = 1000,
    .first_event_ms = 2000,
    .idle_ms = 3000,
  };

  auto anthropic_http = std::make_shared<capture_http_client>(
    "event: message_start\n"
    "data: {\"type\":\"message_start\",\"message\":{\"usage\":{\"input_tokens\":2}}}\n\n"
    "event: message_delta\n"
    "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},"
    "\"usage\":{\"output_tokens\":0}}\n\n"
    "event: message_stop\n"
    "data: {\"type\":\"message_stop\"}\n\n");
  wuwe::anthropic_llm_client anthropic(
    {
      .api_key = "",
      .require_api_key = false,
      .model = "claude-test",
      .stream_timeouts = stream_timeouts,
      .max_retries = 0,
    },
    anthropic_http);
  (void)anthropic.complete_stream(request, {});
  require_stream_timeout_options(anthropic_http->requests.front());

  auto gemini_http = std::make_shared<capture_http_client>(
    "data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"hi\"}]},"
    "\"finishReason\":\"STOP\"}]}\n\n");
  wuwe::gemini_llm_client gemini(
    {
      .api_key = "",
      .require_api_key = false,
      .model = "gemini-test",
      .stream_timeouts = stream_timeouts,
      .max_retries = 0,
    },
    gemini_http);
  (void)gemini.complete_stream(request, {});
  require_stream_timeout_options(gemini_http->requests.front());

  auto ollama_http =
    std::make_shared<capture_http_client>("{\"done\":true,\"done_reason\":\"stop\"}\n");
  wuwe::ollama_llm_client ollama(
    {
      .model = "llama-test",
      .stream_timeouts = stream_timeouts,
      .max_retries = 0,
    },
    ollama_http);
  (void)ollama.complete_stream(request, {});
  require_stream_timeout_options(ollama_http->requests.front());
}

void test_native_provider_streaming_sanitizes_tail_errors_after_output() {
  wuwe::llm_request request;
  request.messages.push_back({ .role = "user", .content = "hello" });

  std::vector<wuwe::llm_stream_event> events;
  wuwe::llm_stream_callbacks callbacks;
  callbacks.on_event = [&](const wuwe::llm_stream_event& event) { events.push_back(event); };

  auto anthropic_http = std::make_shared<streaming_error_http_client>(
    "event: content_block_delta\n"
    "data: "
    "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":"
    "\"hi\"}}\n\n");
  wuwe::anthropic_llm_client anthropic(
    {
      .api_key = "",
      .require_api_key = false,
      .model = "claude-test",
    },
    anthropic_http);
  auto anthropic_response = anthropic.complete_stream(request, callbacks);
  require(!anthropic_response.error_code,
    "Anthropic stream should ignore tail transport error after output");
  require(anthropic_response.content == "hi", "Anthropic stream should retain parsed output");
  require(anthropic_response.metadata.count("ignored_stream_transport_error") == 1,
    "Anthropic stream should record ignored tail transport error");

  auto gemini_http = std::make_shared<streaming_error_http_client>(
    "data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"hi\"}]}}]}\n\n");
  wuwe::gemini_llm_client gemini(
    {
      .api_key = "",
      .require_api_key = false,
      .model = "gemini-test",
    },
    gemini_http);
  auto gemini_response = gemini.complete_stream(request, callbacks);
  require(
    !gemini_response.error_code, "Gemini stream should ignore tail transport error after output");
  require(gemini_response.content == "hi", "Gemini stream should retain parsed output");
  require(gemini_response.metadata.count("ignored_stream_transport_error") == 1,
    "Gemini stream should record ignored tail transport error");

  auto ollama_http = std::make_shared<streaming_error_http_client>(
    "{\"message\":{\"role\":\"assistant\",\"content\":\"hi\"},\"done\":false}\n");
  wuwe::ollama_llm_client ollama(
    {
      .model = "llama-test",
    },
    ollama_http);
  auto ollama_response = ollama.complete_stream(request, callbacks);
  require(
    !ollama_response.error_code, "Ollama stream should ignore tail transport error after output");
  require(ollama_response.content == "hi", "Ollama stream should retain parsed output");
  require(ollama_response.metadata.count("ignored_stream_transport_error") == 1,
    "Ollama stream should record ignored tail transport error");

  for (const auto& event : events) {
    require(event.type != wuwe::llm_stream_event_type::error,
      "tail transport errors after output should not emit error events");
  }
}

void test_native_provider_streaming_invalid_events_are_sanitized() {
  wuwe::llm_request request;
  request.messages.push_back({ .role = "user", .content = "hello" });

  std::vector<wuwe::llm_stream_event> events;
  wuwe::llm_stream_callbacks callbacks;
  callbacks.on_event = [&](const wuwe::llm_stream_event& event) { events.push_back(event); };

  auto anthropic_http = std::make_shared<capture_http_client>("data: not-json\n\n");
  wuwe::anthropic_llm_client anthropic(
    {
      .api_key = "",
      .require_api_key = false,
      .model = "claude-test",
    },
    anthropic_http);
  auto anthropic_response = anthropic.complete_stream(request, callbacks);
  require(anthropic_response.error_code == wuwe::agent::llm_error_code::invalid_response,
    "Anthropic invalid stream should fail");
  require(anthropic_response.content.find("not-json") == std::string::npos,
    "Anthropic invalid stream should not expose raw event data");

  auto gemini_http = std::make_shared<capture_http_client>("data: not-json\n\n");
  wuwe::gemini_llm_client gemini(
    {
      .api_key = "",
      .require_api_key = false,
      .model = "gemini-test",
    },
    gemini_http);
  auto gemini_response = gemini.complete_stream(request, callbacks);
  require(gemini_response.error_code == wuwe::agent::llm_error_code::invalid_response,
    "Gemini invalid stream should fail");
  require(gemini_response.content.find("not-json") == std::string::npos,
    "Gemini invalid stream should not expose raw event data");

  auto ollama_http = std::make_shared<capture_http_client>("not-json\n");
  wuwe::ollama_llm_client ollama(
    {
      .model = "llama-test",
    },
    ollama_http);
  auto ollama_response = ollama.complete_stream(request, callbacks);
  require(ollama_response.error_code == wuwe::agent::llm_error_code::invalid_response,
    "Ollama invalid stream should fail");
  require(ollama_response.content.find("not-json") == std::string::npos,
    "Ollama invalid stream should not expose raw event data");

  for (const auto& event : events) {
    if (event.type == wuwe::llm_stream_event_type::error) {
      require(event.message.find("not-json") == std::string::npos,
        "invalid stream error callbacks should not expose raw event data");
    }
  }
}

void test_native_provider_streaming_ignores_invalid_tail_events_after_output() {
  wuwe::llm_request request;
  request.messages.push_back({ .role = "user", .content = "hello" });

  std::vector<wuwe::llm_stream_event> events;
  wuwe::llm_stream_callbacks callbacks;
  callbacks.on_event = [&](const wuwe::llm_stream_event& event) { events.push_back(event); };

  auto anthropic_http =
    std::make_shared<capture_http_client>("event: content_block_delta\n"
                                          "data: "
                                          "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":"
                                          "{\"type\":\"text_delta\",\"text\":\"hi\"}}\n\n"
                                          "data: not-json\n\n");
  wuwe::anthropic_llm_client anthropic(
    {
      .api_key = "",
      .require_api_key = false,
      .model = "claude-test",
    },
    anthropic_http);
  auto anthropic_response = anthropic.complete_stream(request, callbacks);
  require(!anthropic_response.error_code,
    "Anthropic stream should ignore invalid tail event after output");
  require(anthropic_response.content == "hi",
    "Anthropic stream should retain output before invalid tail event");
  require(anthropic_response.metadata.count("ignored_invalid_stream_event") == 1,
    "Anthropic stream should record ignored invalid tail event");

  auto gemini_http = std::make_shared<capture_http_client>(
    "data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"hi\"}]}}]}\n\n"
    "data: not-json\n\n");
  wuwe::gemini_llm_client gemini(
    {
      .api_key = "",
      .require_api_key = false,
      .model = "gemini-test",
    },
    gemini_http);
  auto gemini_response = gemini.complete_stream(request, callbacks);
  require(
    !gemini_response.error_code, "Gemini stream should ignore invalid tail event after output");
  require(gemini_response.content == "hi",
    "Gemini stream should retain output before invalid tail event");
  require(gemini_response.metadata.count("ignored_invalid_stream_event") == 1,
    "Gemini stream should record ignored invalid tail event");

  auto ollama_http = std::make_shared<capture_http_client>(
    "{\"message\":{\"role\":\"assistant\",\"content\":\"hi\"},\"done\":false}\n"
    "not-json\n");
  wuwe::ollama_llm_client ollama(
    {
      .model = "llama-test",
    },
    ollama_http);
  auto ollama_response = ollama.complete_stream(request, callbacks);
  require(
    !ollama_response.error_code, "Ollama stream should ignore invalid tail event after output");
  require(ollama_response.content == "hi",
    "Ollama stream should retain output before invalid tail event");
  require(ollama_response.metadata.count("ignored_invalid_stream_event") == 1,
    "Ollama stream should record ignored invalid tail event");

  for (const auto& event : events) {
    require(event.type != wuwe::llm_stream_event_type::error,
      "invalid tail events after output should not emit error events");
  }
}

void test_native_provider_retries_before_output() {
  wuwe::llm_request request;
  request.messages.push_back({ .role = "user", .content = "hello" });

  auto retry_http = std::make_shared<capture_http_client>(std::vector<wuwe::http_response> {
    {
      .error_code = make_error_code(wuwe::http_status_code::too_many_requests),
      .status_code = 429,
      .headers = { { .name = "Retry-After", .value = "1" } },
      .body = R"({"error":{"type":"rate_limit_error"}})",
    },
    {
      .body = R"({"message":{"role":"assistant","content":"ok"},"done_reason":"stop"})",
    },
  });
  wuwe::ollama_llm_client ollama(
    {
      .model = "llama-test",
      .max_retries = 1,
      .retry_backoff_ms = 1,
      .retry_max_server_delay_ms = 10,
      .retry_jitter_ratio = 0.0,
    },
    retry_http);
  const auto started = std::chrono::steady_clock::now();
  const auto response = ollama.complete(request);
  const auto elapsed = std::chrono::steady_clock::now() - started;
  require(!response.error_code, "Native client should retry retryable non-streaming failures");
  require(response.content == "ok", "Native retry should return the successful response");
  require(retry_http->requests.size() == 2, "Native retry should issue a second request");
  require(elapsed >= std::chrono::milliseconds(8),
    "native retry should honor Retry-After up to the configured server-delay cap");
}

} // namespace

int main() {
  try {
    test_factory_registers_protocol_and_provider_clients();
    test_factory_preserves_gmp_registration_extension();
    test_provider_registry_exposes_default_metadata_and_config();
    test_aggregate_header_preserves_tool_reflection();
    test_openai_compatible_provider_presets();
    test_new_provider_presets();
    test_priority_provider_protocols();
    test_native_provider_clients_parse_text_and_tools();
    test_execution_context_trace_reaches_provider_http_requests();
    test_reasoning_language_contract_is_mapped_to_provider_payloads();
    test_openai_compatible_replays_reasoning_content();
    test_deepseek_normalizes_validated_dsml_tool_calls();
    test_deepseek_rejects_unregistered_or_malformed_dsml();
    test_deepseek_negotiates_unsupported_explicit_tool_choice();
    test_deepseek_maps_request_scoped_thinking_control();
    test_deepseek_streams_while_filtering_text_tool_protocol();
    test_deepseek_stream_filter_restores_invalid_protocol_text();
    test_openai_compatible_rejects_reasoning_only_terminal_stream();
    test_advanced_generation_capabilities_are_mapped_or_rejected();
    test_native_provider_streaming_success_and_incomplete_streams();
    test_native_provider_streaming_uses_stage_timeout_options();
    test_native_provider_streaming_sanitizes_tail_errors_after_output();
    test_native_provider_streaming_invalid_events_are_sanitized();
    test_native_provider_streaming_ignores_invalid_tail_events_after_output();
    test_native_provider_retries_before_output();
  }
  catch (const std::exception& ex) {
    std::cerr << ex.what() << "\n";
    return 1;
  }
  return 0;
}
