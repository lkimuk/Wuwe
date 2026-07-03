#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
  explicit capture_http_client(std::string body)
      : responses_({{ .body = std::move(body) }}) {}

  explicit capture_http_client(std::vector<wuwe::http_response> responses)
      : responses_(std::move(responses)) {}

  wuwe::http_response send(const wuwe::http_request& request) override {
    requests.push_back(request);
    return next_response();
  }

  wuwe::http_response send_stream(
    const wuwe::http_request& request,
    const wuwe::http_stream_chunk_callback& on_chunk,
    std::stop_token = {}) override {
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
  explicit streaming_error_http_client(std::string body)
      : body_(std::move(body)) {}

  wuwe::http_response send(const wuwe::http_request& request) override {
    requests.push_back(request);
    return {};
  }

  wuwe::http_response send_stream(
    const wuwe::http_request& request,
    const wuwe::http_stream_chunk_callback& on_chunk,
    std::stop_token = {}) override {
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
  auto openai_compatible = factory.create_shared("OpenAICompatible", wuwe::llm_client_config {
    .base_url = "https://example.test",
    .api_key = "",
    .require_api_key = false,
    .model = "test-model",
    .max_retries = 0,
  });
  require(static_cast<bool>(openai_compatible),
    "factory should create OpenAICompatible protocol client");
  require(openai_compatible->supports_streaming(),
    "OpenAICompatible protocol client should support streaming");

  auto openrouter = factory.create_shared("OpenRouter", wuwe::llm_client_config {
    .api_key = "",
    .require_api_key = false,
    .model = "test-model",
    .max_retries = 0,
  });
  require(static_cast<bool>(openrouter), "factory should create OpenRouter provider preset");
  require(openrouter->supports_streaming(), "OpenRouter provider preset should support streaming");

  auto via_helper = wuwe::make_llm_client("OpenAI", wuwe::llm_client_config {
    .api_key = "",
    .require_api_key = false,
    .model = "test-model",
    .max_retries = 0,
  });
  require(static_cast<bool>(via_helper), "make_llm_client should create provider clients");

  auto via_singleton = wuwe::llm_client_factory::instance().create_shared(
    "Gemini",
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
       }) {
    auto client = factory.create_shared(key, wuwe::llm_client_config {
      .api_key = "",
      .require_api_key = false,
      .model = "test-model",
      .max_retries = 0,
    });
    require(static_cast<bool>(client), std::string("factory should create ") + key);
    require(client->supports_streaming(), std::string(key) + " should support streaming");
  }
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
  require(openai->capabilities.streaming && openai->capabilities.tools,
    "OpenAI metadata should expose core chat capabilities");
  require(!openai->capabilities.streaming_reasoning_summary,
    "OpenAI chat-completions metadata should not advertise reasoning streams by default");
  require(wuwe::to_string(openai->protocol) == "openai_compatible",
    "provider protocol should have a stable string form");

  const auto* compatible = wuwe::find_llm_provider("OpenAICompatible");
  require(compatible != nullptr, "provider registry should expose OpenAICompatible");
  require(compatible->default_base_url.empty(),
    "generic OpenAI-compatible metadata should not invent a default base URL");
  require(compatible->base_url_required,
    "generic OpenAI-compatible metadata should mark base URL as required");

  const auto* ollama = wuwe::find_llm_provider("Ollama");
  require(ollama != nullptr, "provider registry should expose Ollama");
  require(ollama->default_base_url == "http://localhost:11434",
    "Ollama metadata should expose its local base URL");
  require(!ollama->api_key_required,
    "Ollama metadata should not require an API key by default");
  require(ollama->capabilities.local_runtime,
    "Ollama metadata should identify local runtime providers");
  require(ollama->capabilities.streaming_reasoning_summary,
    "Ollama metadata should expose provider-native thinking streams when models return them");

  const auto* deepseek = wuwe::find_llm_provider("DeepSeek");
  require(deepseek != nullptr, "provider registry should expose DeepSeek");
  require(deepseek->capabilities.streaming_reasoning_summary,
    "DeepSeek metadata should expose provider-native reasoning streams");

  const auto anthropic_config = wuwe::make_default_llm_config("Anthropic");
  require(anthropic_config.has_value(),
    "provider registry should build default config for known providers");
  require(anthropic_config->base_url == "https://api.anthropic.com",
    "default config should carry provider base URL");
  require(anthropic_config->chat_completions_path.empty(),
    "native provider default config should not expose an OpenAI chat path");
  require(anthropic_config->require_api_key,
    "default config should carry provider API-key policy");

  auto default_normalized = wuwe::normalize_llm_client_config("OpenAI", wuwe::llm_client_config {
    .api_key = "explicit",
    .model = "model",
  });
  require(default_normalized.has_value(), "provider registry should normalize OpenAI");
  require(default_normalized->base_url == "https://api.openai.com",
    "normalization should apply provider default base URL");

  auto normalized = wuwe::normalize_llm_client_config("OpenAI", wuwe::llm_client_config {
    .base_url = "https://proxy.example",
    .api_key = "explicit",
    .model = "model",
  });
  require(normalized.has_value(), "provider registry should normalize known providers");
  require(normalized->base_url == "https://proxy.example",
    "normalization should preserve explicit base URL overrides");
  require(normalized->api_key == "explicit",
    "normalization should preserve explicit API keys");

  auto local_normalized = wuwe::normalize_llm_client_config("Ollama", wuwe::llm_client_config {
    .model = "llama",
  });
  require(local_normalized.has_value(), "provider registry should normalize Ollama");
  require(!local_normalized->require_api_key,
    "normalization should apply provider API-key policy");

  const auto* qwen = wuwe::find_llm_provider("Qwen");
  require(qwen != nullptr, "provider registry should expose Qwen");
  require(qwen->api_key_env_names.size() >= 2 && qwen->api_key_env_names[0] == "QWEN_API_KEY",
    "Qwen metadata should prefer QWEN_API_KEY");

  const auto* zhipu = wuwe::find_llm_provider("Zhipu");
  require(zhipu != nullptr, "provider registry should expose Zhipu");
  require(zhipu->display_name == "Zhipu GLM",
    "Zhipu metadata should expose a user-facing display name");
  require(zhipu->default_base_url == "https://open.bigmodel.cn/api/paas/v4",
    "Zhipu metadata should expose the BigModel base URL");
  require(zhipu->default_chat_completions_path == "/chat/completions",
    "Zhipu metadata should expose its non-v1 chat completions path");
  require(zhipu->api_key_env_names.size() >= 2 &&
      zhipu->api_key_env_names[0] == "ZHIPU_API_KEY" &&
      zhipu->api_key_env_names[1] == "BIGMODEL_API_KEY",
    "Zhipu metadata should expose stable API key env names");

  const auto zhipu_config = wuwe::make_default_llm_config("Zhipu");
  require(zhipu_config.has_value(), "provider registry should build default config for Zhipu");
  require(zhipu_config->base_url == "https://open.bigmodel.cn/api/paas/v4",
    "Zhipu default config should carry the BigModel base URL");
  require(zhipu_config->chat_completions_path == "/chat/completions",
    "Zhipu default config should carry the provider-specific chat path");

  require(!wuwe::find_llm_provider("Missing"),
    "provider registry should report missing providers");
  require(!wuwe::make_default_llm_config("Missing").has_value(),
    "provider registry should not build config for missing providers");
}

void test_aggregate_header_preserves_tool_reflection() {
  const auto tool = wuwe::make_llm_tool<aggregate_header_echo>();
  require(tool.name == "aggregate_header_echo",
    "wuwe.h should not interfere with reflected tool names");
  require(tool.description == aggregate_header_echo::description,
    "wuwe.h should not interfere with reflected tool descriptions");
  require(tool.parameters_json_schema.find("\"text\"") != std::string::npos,
    "wuwe.h should not interfere with reflected tool parameters");
  require(tool.parameters_json_schema.find("Text to echo.") != std::string::npos,
    "wuwe.h should not interfere with field descriptions");
}

void test_openai_compatible_provider_presets() {
  const std::string openai_body =
    R"({"choices":[{"message":{"content":"ok"},"finish_reason":"stop"}]})";
  wuwe::llm_request request;
  request.messages.push_back({ .role = "user", .content = "hello" });

  auto custom_path_http = std::make_shared<capture_http_client>(openai_body);
  wuwe::openai_compatible_llm_client custom_path_client({
    .base_url = "https://gateway.example/api",
    .chat_completions_path = "custom/chat",
    .api_key = "",
    .require_api_key = false,
    .model = "compatible-test",
  }, custom_path_http);
  (void)custom_path_client.complete(request);
  require(custom_path_http->requests.front().url ==
      "https://gateway.example/api/custom/chat",
    "OpenAI-compatible client should honor a custom chat completions path");

  auto trailing_slash_http = std::make_shared<capture_http_client>(openai_body);
  wuwe::openai_compatible_llm_client trailing_slash_client({
    .base_url = "https://gateway.example/api/",
    .chat_completions_path = "/custom/chat",
    .api_key = "",
    .require_api_key = false,
    .model = "compatible-test",
  }, trailing_slash_http);
  (void)trailing_slash_client.complete(request);
  require(trailing_slash_http->requests.front().url ==
      "https://gateway.example/api/custom/chat",
    "OpenAI-compatible client should avoid double slashes when joining URL paths");

  auto openai_http = std::make_shared<capture_http_client>(openai_body);
  wuwe::openai_llm_client openai({
    .api_key = "",
    .require_api_key = false,
    .model = "gpt-test",
  }, openai_http);
  require(openai.complete(request).content == "ok", "OpenAI preset should parse response");
  require(openai_http->requests.front().url == "https://api.openai.com/v1/chat/completions",
    "OpenAI preset should use the OpenAI base URL");

  auto deepseek_http = std::make_shared<capture_http_client>(openai_body);
  wuwe::deepseek_llm_client deepseek({
    .api_key = "",
    .require_api_key = false,
    .model = "deepseek-test",
  }, deepseek_http);
  (void)deepseek.complete(request);
  require(deepseek_http->requests.front().url == "https://api.deepseek.com/v1/chat/completions",
    "DeepSeek preset should use the DeepSeek base URL");

  auto dashscope_http = std::make_shared<capture_http_client>(openai_body);
  wuwe::dashscope_llm_client dashscope({
    .api_key = "",
    .require_api_key = false,
    .model = "qwen-test",
  }, dashscope_http);
  (void)dashscope.complete(request);
  require(dashscope_http->requests.front().url ==
      "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions",
    "DashScope preset should use the compatible-mode base URL");

  auto qwen_http = std::make_shared<capture_http_client>(openai_body);
  wuwe::qwen_llm_client qwen({
    .api_key = "",
    .require_api_key = false,
    .model = "qwen-test",
  }, qwen_http);
  (void)qwen.complete(request);
  require(qwen_http->requests.front().url ==
      "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions",
    "Qwen preset should use the DashScope compatible-mode base URL");

  auto zhipu_http = std::make_shared<capture_http_client>(openai_body);
  wuwe::zhipu_llm_client zhipu({
    .api_key = "",
    .require_api_key = false,
    .model = "glm-test",
  }, zhipu_http);
  (void)zhipu.complete(request);
  require(zhipu_http->requests.front().url ==
      "https://open.bigmodel.cn/api/paas/v4/chat/completions",
    "Zhipu preset should use the BigModel v4 chat completions URL");

  auto zhipu_stream_http = std::make_shared<capture_http_client>(
    "data: {\"choices\":[{\"delta\":{\"content\":\"hi\"},\"finish_reason\":\"stop\"}]}\n\n"
    "data: [DONE]\n\n");
  wuwe::zhipu_llm_client zhipu_stream({
    .api_key = "",
    .require_api_key = false,
    .model = "glm-test",
  }, zhipu_stream_http);
  std::vector<wuwe::llm_stream_event> events;
  wuwe::llm_stream_callbacks callbacks;
  callbacks.on_event = [&](const wuwe::llm_stream_event& event) {
    events.push_back(event);
  };
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
    R"({"content":[{"type":"thinking","thinking":"inspect","signature":"sig-a"},{"type":"text","text":"hi"},{"type":"tool_use","id":"t1","name":"lookup","input":{"q":"x"}}],"stop_reason":"tool_use","usage":{"input_tokens":2,"output_tokens":3}})");
  wuwe::anthropic_llm_client anthropic({
    .api_key = "",
    .require_api_key = false,
    .model = "claude-test",
  }, anthropic_http);
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
  require(anthropic_response.tool_calls.size() == 1,
    "Anthropic client should parse tool_use blocks");

  auto gemini_http = std::make_shared<capture_http_client>(
    R"({"candidates":[{"content":{"parts":[{"text":"inspect","thought":true,"thoughtSignature":"sig-g"},{"text":"hi"},{"functionCall":{"name":"lookup","args":{"q":"x"}}}]},"finishReason":"STOP"}],"usageMetadata":{"promptTokenCount":2,"candidatesTokenCount":3,"totalTokenCount":5}})");
  wuwe::gemini_llm_client gemini({
    .api_key = "",
    .require_api_key = false,
    .model = "gemini-test",
  }, gemini_http);
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
  require(gemini_response.tool_calls.size() == 1,
    "Gemini client should parse functionCall parts");

  auto ollama_http = std::make_shared<capture_http_client>(
    R"({"message":{"role":"assistant","thinking":"inspect","content":"hi","tool_calls":[{"function":{"name":"lookup","arguments":{"q":"x"}}}]},"done_reason":"stop","prompt_eval_count":2,"eval_count":3})");
  wuwe::ollama_llm_client ollama({
    .model = "llama-test",
  }, ollama_http);
  const auto ollama_response = ollama.complete(request);
  require(ollama_http->requests.front().url == "http://localhost:11434/api/chat",
    "Ollama client should use the local chat API URL");
  require(ollama_response.content == "hi", "Ollama client should parse message content");
  require(ollama_response.reasoning_summary == "inspect",
    "Ollama client should parse thinking content separately");
  require(ollama_response.tool_calls.size() == 1,
    "Ollama client should parse tool_calls");
}

void test_native_provider_streaming_success_and_incomplete_streams() {
  wuwe::llm_request request;
  request.messages.push_back({ .role = "user", .content = "hello" });

  std::vector<wuwe::llm_stream_event> events;
  wuwe::llm_stream_callbacks callbacks;
  callbacks.on_event = [&](const wuwe::llm_stream_event& event) {
    events.push_back(event);
  };

  auto anthropic_http = std::make_shared<capture_http_client>(
    "event: message_start\n"
    "data: {\"type\":\"message_start\",\"message\":{\"usage\":{\"input_tokens\":2}}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"inspect\"}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"signature_delta\",\"signature\":\"sig-a\"}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"hi\"}}\n\n"
    "event: message_delta\n"
    "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},\"usage\":{\"output_tokens\":3}}\n\n"
    "event: message_stop\n"
    "data: {\"type\":\"message_stop\"}\n\n");
  wuwe::anthropic_llm_client anthropic({
    .api_key = "",
    .require_api_key = false,
    .model = "claude-test",
  }, anthropic_http);
  auto anthropic_response = anthropic.complete_stream(request, callbacks);
  require(!anthropic_response.error_code, "Anthropic stream should succeed");
  require(anthropic_response.content == "hi", "Anthropic stream should aggregate text");
  require(anthropic_response.reasoning_summary == "inspect",
    "Anthropic stream should aggregate thinking deltas separately");
  require(anthropic_response.reasoning_metadata.at("signature") == "sig-a",
    "Anthropic stream should preserve thinking signature metadata");
  require(anthropic_response.usage.total_tokens == 5, "Anthropic stream should parse usage");

  auto anthropic_incomplete_http = std::make_shared<capture_http_client>(
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"partial\"}}\n\n");
  wuwe::anthropic_llm_client anthropic_incomplete({
    .api_key = "",
    .require_api_key = false,
    .model = "claude-test",
  }, anthropic_incomplete_http);
  auto anthropic_incomplete_response = anthropic_incomplete.complete_stream(request, callbacks);
  require(anthropic_incomplete_response.error_code == wuwe::agent::llm_error_code::invalid_response,
    "Anthropic incomplete stream should fail");

  auto gemini_http = std::make_shared<capture_http_client>(
    "data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"inspect\",\"thought\":true,\"thoughtSignature\":\"sig-g\"},{\"text\":\"hi\"}]},\"finishReason\":\"STOP\"}],"
    "\"usageMetadata\":{\"promptTokenCount\":2,\"candidatesTokenCount\":3,\"totalTokenCount\":5}}\n\n");
  wuwe::gemini_llm_client gemini({
    .api_key = "",
    .require_api_key = false,
    .model = "gemini-test",
  }, gemini_http);
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
  wuwe::gemini_llm_client gemini_incomplete({
    .api_key = "",
    .require_api_key = false,
    .model = "gemini-test",
  }, gemini_incomplete_http);
  auto gemini_incomplete_response = gemini_incomplete.complete_stream(request, callbacks);
  require(gemini_incomplete_response.error_code == wuwe::agent::llm_error_code::invalid_response,
    "Gemini incomplete stream should fail");

  auto ollama_http = std::make_shared<capture_http_client>(
    "{\"message\":{\"role\":\"assistant\",\"thinking\":\"inspect\",\"content\":\"hi\"},\"done\":false}\n"
    "{\"done\":true,\"done_reason\":\"stop\",\"prompt_eval_count\":2,\"eval_count\":3}\n");
  wuwe::ollama_llm_client ollama({
    .model = "llama-test",
  }, ollama_http);
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
      require(event.content_delta.empty(),
        "provider reasoning events should not carry content deltas");
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
  wuwe::ollama_llm_client ollama_incomplete({
    .model = "llama-test",
  }, ollama_incomplete_http);
  auto ollama_incomplete_response = ollama_incomplete.complete_stream(request, callbacks);
  require(ollama_incomplete_response.error_code == wuwe::agent::llm_error_code::invalid_response,
    "Ollama incomplete stream should fail");
}

void require_stream_timeout_options(const wuwe::http_request& request) {
  require(request.timeouts.total_ms == 9000,
    "streaming total timeout should map to HTTP total timeout");
  require(request.timeouts.connect_ms == 1000,
    "streaming connect timeout should map to HTTP connect timeout");
  require(request.timeouts.read_ms == 3000,
    "streaming idle timeout should map to HTTP read timeout");
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
  wuwe::anthropic_llm_client anthropic({
    .api_key = "",
    .require_api_key = false,
    .model = "claude-test",
    .stream_timeouts = stream_timeouts,
    .max_retries = 0,
  }, anthropic_http);
  (void)anthropic.complete_stream(request, {});
  require_stream_timeout_options(anthropic_http->requests.front());

  auto gemini_http = std::make_shared<capture_http_client>(
    "data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"hi\"}]},"
    "\"finishReason\":\"STOP\"}]}\n\n");
  wuwe::gemini_llm_client gemini({
    .api_key = "",
    .require_api_key = false,
    .model = "gemini-test",
    .stream_timeouts = stream_timeouts,
    .max_retries = 0,
  }, gemini_http);
  (void)gemini.complete_stream(request, {});
  require_stream_timeout_options(gemini_http->requests.front());

  auto ollama_http = std::make_shared<capture_http_client>(
    "{\"done\":true,\"done_reason\":\"stop\"}\n");
  wuwe::ollama_llm_client ollama({
    .model = "llama-test",
    .stream_timeouts = stream_timeouts,
    .max_retries = 0,
  }, ollama_http);
  (void)ollama.complete_stream(request, {});
  require_stream_timeout_options(ollama_http->requests.front());
}

void test_native_provider_streaming_sanitizes_tail_errors_after_output() {
  wuwe::llm_request request;
  request.messages.push_back({ .role = "user", .content = "hello" });

  std::vector<wuwe::llm_stream_event> events;
  wuwe::llm_stream_callbacks callbacks;
  callbacks.on_event = [&](const wuwe::llm_stream_event& event) {
    events.push_back(event);
  };

  auto anthropic_http = std::make_shared<streaming_error_http_client>(
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"hi\"}}\n\n");
  wuwe::anthropic_llm_client anthropic({
    .api_key = "",
    .require_api_key = false,
    .model = "claude-test",
  }, anthropic_http);
  auto anthropic_response = anthropic.complete_stream(request, callbacks);
  require(!anthropic_response.error_code,
    "Anthropic stream should ignore tail transport error after output");
  require(anthropic_response.content == "hi", "Anthropic stream should retain parsed output");
  require(anthropic_response.metadata.count("ignored_stream_transport_error") == 1,
    "Anthropic stream should record ignored tail transport error");

  auto gemini_http = std::make_shared<streaming_error_http_client>(
    "data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"hi\"}]}}]}\n\n");
  wuwe::gemini_llm_client gemini({
    .api_key = "",
    .require_api_key = false,
    .model = "gemini-test",
  }, gemini_http);
  auto gemini_response = gemini.complete_stream(request, callbacks);
  require(!gemini_response.error_code,
    "Gemini stream should ignore tail transport error after output");
  require(gemini_response.content == "hi", "Gemini stream should retain parsed output");
  require(gemini_response.metadata.count("ignored_stream_transport_error") == 1,
    "Gemini stream should record ignored tail transport error");

  auto ollama_http = std::make_shared<streaming_error_http_client>(
    "{\"message\":{\"role\":\"assistant\",\"content\":\"hi\"},\"done\":false}\n");
  wuwe::ollama_llm_client ollama({
    .model = "llama-test",
  }, ollama_http);
  auto ollama_response = ollama.complete_stream(request, callbacks);
  require(!ollama_response.error_code,
    "Ollama stream should ignore tail transport error after output");
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
  callbacks.on_event = [&](const wuwe::llm_stream_event& event) {
    events.push_back(event);
  };

  auto anthropic_http = std::make_shared<capture_http_client>("data: not-json\n\n");
  wuwe::anthropic_llm_client anthropic({
    .api_key = "",
    .require_api_key = false,
    .model = "claude-test",
  }, anthropic_http);
  auto anthropic_response = anthropic.complete_stream(request, callbacks);
  require(anthropic_response.error_code == wuwe::agent::llm_error_code::invalid_response,
    "Anthropic invalid stream should fail");
  require(anthropic_response.content.find("not-json") == std::string::npos,
    "Anthropic invalid stream should not expose raw event data");

  auto gemini_http = std::make_shared<capture_http_client>("data: not-json\n\n");
  wuwe::gemini_llm_client gemini({
    .api_key = "",
    .require_api_key = false,
    .model = "gemini-test",
  }, gemini_http);
  auto gemini_response = gemini.complete_stream(request, callbacks);
  require(gemini_response.error_code == wuwe::agent::llm_error_code::invalid_response,
    "Gemini invalid stream should fail");
  require(gemini_response.content.find("not-json") == std::string::npos,
    "Gemini invalid stream should not expose raw event data");

  auto ollama_http = std::make_shared<capture_http_client>("not-json\n");
  wuwe::ollama_llm_client ollama({
    .model = "llama-test",
  }, ollama_http);
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
  callbacks.on_event = [&](const wuwe::llm_stream_event& event) {
    events.push_back(event);
  };

  auto anthropic_http = std::make_shared<capture_http_client>(
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"hi\"}}\n\n"
    "data: not-json\n\n");
  wuwe::anthropic_llm_client anthropic({
    .api_key = "",
    .require_api_key = false,
    .model = "claude-test",
  }, anthropic_http);
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
  wuwe::gemini_llm_client gemini({
    .api_key = "",
    .require_api_key = false,
    .model = "gemini-test",
  }, gemini_http);
  auto gemini_response = gemini.complete_stream(request, callbacks);
  require(!gemini_response.error_code,
    "Gemini stream should ignore invalid tail event after output");
  require(gemini_response.content == "hi",
    "Gemini stream should retain output before invalid tail event");
  require(gemini_response.metadata.count("ignored_invalid_stream_event") == 1,
    "Gemini stream should record ignored invalid tail event");

  auto ollama_http = std::make_shared<capture_http_client>(
    "{\"message\":{\"role\":\"assistant\",\"content\":\"hi\"},\"done\":false}\n"
    "not-json\n");
  wuwe::ollama_llm_client ollama({
    .model = "llama-test",
  }, ollama_http);
  auto ollama_response = ollama.complete_stream(request, callbacks);
  require(!ollama_response.error_code,
    "Ollama stream should ignore invalid tail event after output");
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
      .body = R"({"error":{"type":"rate_limit_error"}})",
    },
    {
      .body = R"({"message":{"role":"assistant","content":"ok"},"done_reason":"stop"})",
    },
  });
  wuwe::ollama_llm_client ollama({
    .model = "llama-test",
    .max_retries = 1,
    .retry_backoff_ms = 1,
  }, retry_http);
  const auto response = ollama.complete(request);
  require(!response.error_code, "Native client should retry retryable non-streaming failures");
  require(response.content == "ok", "Native retry should return the successful response");
  require(retry_http->requests.size() == 2, "Native retry should issue a second request");
}

} // namespace

int main() {
  try {
    test_factory_registers_protocol_and_provider_clients();
    test_provider_registry_exposes_default_metadata_and_config();
    test_aggregate_header_preserves_tool_reflection();
    test_openai_compatible_provider_presets();
    test_native_provider_clients_parse_text_and_tools();
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
