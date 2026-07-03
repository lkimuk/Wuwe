#include <wuwe/agent/llm/gemini_llm_client.h>

#include "llm_retry.hpp"
#include "llm_stream_timeouts.hpp"

#include <wuwe/agent/llm/llm_error.h>
#include <wuwe/agent/llm/llm_provider_registry.h>
#include <wuwe/net/default_http_client.h>
#include <wuwe/net/net_errc.h>
#include <wuwe/net/sse_event_parser.h>

#include <utility>

WUWE_NAMESPACE_BEGIN

namespace {

using json = nlohmann::json;

json parse_json_object_or_default(const std::string& text) {
  const auto parsed = json::parse(text, nullptr, false);
  return parsed.is_object() ? parsed : json::object();
}

std::error_code classify_gemini_error(const std::error_code& transport_or_http_error,
  const json& body) {
  if (transport_or_http_error == net_errc::unauthorized ||
      transport_or_http_error == net_errc::forbidden) {
    return agent::make_error_code(agent::llm_error_code::authentication_failed);
  }
  if (transport_or_http_error == net_errc::rate_limited) {
    return agent::make_error_code(agent::llm_error_code::rate_limited);
  }
  if (transport_or_http_error == net_errc::timeout) {
    return agent::make_error_code(agent::llm_error_code::timeout);
  }
  if (transport_or_http_error == net_errc::not_found) {
    return agent::make_error_code(agent::llm_error_code::model_unavailable);
  }
  if (body.is_object() && body.contains("error") && body["error"].is_object()) {
    const auto status = body["error"].value("status", std::string {});
    if (status == "UNAUTHENTICATED" || status == "PERMISSION_DENIED") {
      return agent::make_error_code(agent::llm_error_code::authentication_failed);
    }
    if (status == "RESOURCE_EXHAUSTED") {
      return agent::make_error_code(agent::llm_error_code::rate_limited);
    }
    if (status == "NOT_FOUND") {
      return agent::make_error_code(agent::llm_error_code::model_unavailable);
    }
    return agent::make_error_code(agent::llm_error_code::api_error);
  }
  return transport_or_http_error;
}

std::string gemini_error_message(const std::error_code& error_code, const json& body) {
  if (body.is_object() && body.contains("error") && body["error"].is_object()) {
    const auto& error = body["error"];
    if (error.contains("message") && error["message"].is_string()) {
      return error["message"].get<std::string>();
    }
    const auto status = error.value("status", std::string {});
    if (!status.empty()) {
      return "Gemini API error: " + status;
    }
  }
  if (error_code) {
    return "Gemini streaming request failed: " + error_code.message();
  }
  return "Gemini streaming request failed.";
}

std::string gemini_model_name(const std::string& model) {
  constexpr std::string_view prefix = "models/";
  if (model.rfind(prefix, 0) == 0) {
    return model.substr(prefix.size());
  }
  return model;
}

void append_candidate(llm_response& result, const json& candidate) {
  if (candidate.contains("finishReason") && candidate["finishReason"].is_string()) {
    result.finish_reason = candidate["finishReason"].get<std::string>();
  }
  if (!candidate.contains("content") || !candidate["content"].is_object()) {
    return;
  }
  const auto& content = candidate["content"];
  if (!content.contains("parts") || !content["parts"].is_array()) {
    return;
  }
  for (const auto& part : content["parts"]) {
    if (!part.is_object()) {
      continue;
    }
    if (part.contains("text") && part["text"].is_string()) {
      if (part.value("thought", false)) {
        result.reasoning_summary += part["text"].get<std::string>();
      }
      else {
        result.content += part["text"].get<std::string>();
      }
      if (part.contains("thoughtSignature") && part["thoughtSignature"].is_string()) {
        result.reasoning_metadata["thought_signature"] =
          part["thoughtSignature"].get<std::string>();
      }
    }
    else if (part.contains("functionCall") && part["functionCall"].is_object()) {
      const auto& function = part["functionCall"];
      result.tool_calls.push_back({
        .id = function.value("id", std::string {}),
        .name = function.value("name", std::string {}),
        .arguments_json = function.contains("args") ? function["args"].dump() : std::string("{}"),
      });
    }
  }
}

void emit_stream_event(const llm_stream_callbacks& callbacks, llm_stream_event event) {
  if (callbacks.on_event) {
    callbacks.on_event(event);
  }
  if (event.type == llm_stream_event_type::reasoning_delta &&
      callbacks.on_reasoning_delta && !event.reasoning_delta.empty()) {
    callbacks.on_reasoning_delta(event.reasoning_delta);
  }
  if (event.type == llm_stream_event_type::reasoning_done &&
      callbacks.on_reasoning_done && !event.reasoning_summary.empty()) {
    callbacks.on_reasoning_done(event.reasoning_summary);
  }
}

} // namespace

gemini_llm_client::gemini_llm_client(llm_client_config config)
    : config_(normalize_config(std::move(config))),
      http_(std::make_shared<default_http_client>()) {}

gemini_llm_client::gemini_llm_client(
  llm_client_config config,
  std::shared_ptr<http_client> http)
    : config_(normalize_config(std::move(config))),
      http_(std::move(http)) {
  if (!http_) {
    http_ = std::make_shared<default_http_client>();
  }
}

llm_client_config gemini_llm_client::normalize_config(llm_client_config config) {
  if (auto normalized = normalize_llm_client_config("Gemini", config)) {
    return std::move(*normalized);
  }
  return config;
}

json gemini_llm_client::build_payload(const llm_request& request) const {
  auto contents = json::array();
  std::string system = llm_language_contract(request.language);

  for (const auto& msg : request.messages) {
    if (msg.role == "system") {
      if (!system.empty()) {
        system += "\n\n";
      }
      system += msg.content;
      continue;
    }

    json content;
    content["role"] = msg.role == "assistant" ? "model" : "user";
    auto parts = json::array();
    if (msg.role == "tool") {
      parts.push_back({
        {"functionResponse", {
          {"name", msg.name.value_or("tool")},
          {"response", {{"content", msg.content}}}
        }}
      });
    }
    else {
      if (!msg.content.empty()) {
        parts.push_back({{"text", msg.content}});
      }
      for (const auto& call : msg.tool_calls) {
        parts.push_back({
          {"functionCall", {
            {"id", call.id},
            {"name", call.name},
            {"args", parse_json_object_or_default(call.arguments_json)}
          }}
        });
      }
    }
    content["parts"] = std::move(parts);
    contents.push_back(std::move(content));
  }

  json payload = {
    {"contents", std::move(contents)},
    {"generationConfig", {{"temperature", request.temperature}}},
  };
  if (!system.empty()) {
    payload["systemInstruction"] = {{"parts", json::array({{{"text", system}}})}};
  }
  if (!request.tools.empty()) {
    auto declarations = json::array();
    for (const auto& tool : request.tools) {
      declarations.push_back({
        {"name", tool.name},
        {"description", tool.description},
        {"parameters", parse_json_object_or_default(tool.parameters_json_schema)}
      });
    }
    payload["tools"] = json::array({{{"functionDeclarations", std::move(declarations)}}});
  }
  if (request.tool_choice.has_value()) {
    json config;
    switch (request.tool_choice->mode) {
      case llm_tool_choice_mode::none:
        config["mode"] = "NONE";
        break;
      case llm_tool_choice_mode::required:
        config["mode"] = "ANY";
        break;
      case llm_tool_choice_mode::named:
        config["mode"] = "ANY";
        config["allowedFunctionNames"] = json::array({request.tool_choice->name});
        break;
      default:
        config["mode"] = "AUTO";
        break;
    }
    payload["toolConfig"] = {{"functionCallingConfig", std::move(config)}};
  }
  return payload;
}

std::vector<std::pair<std::string, std::string>> gemini_llm_client::build_headers() const {
  std::vector<std::pair<std::string, std::string>> headers {
    {"Content-Type", "application/json"},
  };
  if (!config_.api_key.empty()) {
    headers.push_back({"x-goog-api-key", config_.api_key});
  }
  return headers;
}

std::string gemini_llm_client::build_url(const llm_request& request, bool stream) const {
  const auto model = gemini_model_name(request.model.empty() ? config_.model : request.model);
  auto url = config_.base_url + "/v1beta/models/" + model +
             (stream ? ":streamGenerateContent?alt=sse" : ":generateContent");
  return url;
}

llm_response gemini_llm_client::parse_response(const http_response& response) const {
  llm_response result;
  const auto data = json::parse(response.body, nullptr, false);
  if (response.error_code) {
    result.content = response.body;
    result.error_code = classify_gemini_error(
      response.error_code,
      data.is_discarded() ? json::object() : data);
    return result;
  }
  if (data.is_discarded() || !data.is_object()) {
    result.content = response.body;
    result.error_code = agent::make_error_code(agent::llm_error_code::invalid_response);
    return result;
  }
  if (data.contains("error")) {
    result.content = response.body;
    result.error_code =
      classify_gemini_error(agent::make_error_code(agent::llm_error_code::api_error), data);
    return result;
  }
  if (data.contains("usageMetadata") && data["usageMetadata"].is_object()) {
    result.usage.prompt_tokens = data["usageMetadata"].value("promptTokenCount", 0);
    result.usage.completion_tokens = data["usageMetadata"].value("candidatesTokenCount", 0);
    result.usage.total_tokens = data["usageMetadata"].value("totalTokenCount", 0);
  }
  if (!data.contains("candidates") || !data["candidates"].is_array() ||
      data["candidates"].empty()) {
    result.content = response.body;
    result.error_code = agent::make_error_code(agent::llm_error_code::invalid_response);
    return result;
  }
  append_candidate(result, data["candidates"].front());
  return result;
}

llm_response gemini_llm_client::complete(const llm_request& request) {
  return complete(request, {});
}

llm_response gemini_llm_client::complete(const llm_request& request, std::stop_token stop_token) {
  if (stop_token.stop_requested()) {
    return { .error_code = agent::make_error_code(agent::llm_error_code::cancelled) };
  }
  if (config_.require_api_key && config_.api_key.empty()) {
    return { .error_code = agent::make_error_code(agent::llm_error_code::missing_api_key) };
  }
  const http_request req {
    .method = "POST",
    .url = build_url(request, false),
    .headers = build_headers(),
    .body = build_payload(request).dump(),
    .timeout = config_.timeout,
  };
  const int max_retries = config_.max_retries < 0 ? 0 : config_.max_retries;
  const int base_backoff_ms = config_.retry_backoff_ms <= 0 ? 500 : config_.retry_backoff_ms;
  for (int attempt = 0; attempt <= max_retries; ++attempt) {
    if (stop_token.stop_requested()) {
      return { .error_code = agent::make_error_code(agent::llm_error_code::cancelled) };
    }
    auto result = parse_response(http_->send(req));
    apply_reasoning_language_metadata(
      result,
      request.language,
      has_language_preferences(request.language)
        ? llm_reasoning_language_control::prompt_contract
        : llm_reasoning_language_control::unsupported);
    if (stop_token.stop_requested() && !result.error_code) {
      result.error_code = agent::make_error_code(agent::llm_error_code::cancelled);
    }
    if (!result.error_code || attempt >= max_retries ||
        !agent::llm_detail::is_retryable_error(result.error_code)) {
      return result;
    }
    if (!agent::llm_detail::wait_for_retry(
          stop_token,
          std::chrono::milliseconds(agent::llm_detail::compute_backoff_ms(
            attempt,
            base_backoff_ms)))) {
      return { .error_code = agent::make_error_code(agent::llm_error_code::cancelled) };
    }
  }
  return { .error_code = std::make_error_code(std::errc::io_error) };
}

llm_response gemini_llm_client::complete_stream(
  const llm_request& request,
  const llm_stream_callbacks& callbacks,
  std::stop_token stop_token) {
  if (stop_token.stop_requested()) {
    return { .error_code = agent::make_error_code(agent::llm_error_code::cancelled) };
  }
  if (config_.require_api_key && config_.api_key.empty()) {
    llm_response response { .error_code = agent::make_error_code(agent::llm_error_code::missing_api_key) };
    emit_stream_event(callbacks, { .type = llm_stream_event_type::error, .response = response, .error_code = response.error_code });
    return response;
  }

  llm_response result;
  const auto reasoning_language_control =
    has_language_preferences(request.language)
      ? llm_reasoning_language_control::prompt_contract
      : llm_reasoning_language_control::unsupported;
  sse_event_parser parser;
  bool emitted_output = false;
  bool saw_event = false;
  bool saw_done = false;
  bool ignored_invalid_stream_event = false;
  agent::llm_detail::stream_timeout_guard timeout_guard(config_.stream_timeouts);
  const auto fail_stream_timeout =
    [&](const agent::llm_detail::stream_timeout& timeout) {
      result.metadata["timeout_phase"] = timeout.phase;
      result.metadata["timeout_ms"] = std::to_string(timeout.timeout_ms);
      result.content = "Gemini streaming " + timeout.phase + " timeout.";
      result.error_code = agent::make_error_code(agent::llm_error_code::timeout);
      return false;
    };
  const auto process_event = [&](const sse_event& event) {
    if (event.data.empty()) {
      return true;
    }
    if (const auto timeout = timeout_guard.check_before_event()) {
      return fail_stream_timeout(*timeout);
    }
    timeout_guard.mark_event();
    saw_event = true;
    const auto data = json::parse(event.data, nullptr, false);
    if (data.is_discarded() || !data.is_object()) {
      if (emitted_output) {
        ignored_invalid_stream_event = true;
        result.metadata["ignored_invalid_stream_event"] = "true";
        return true;
      }
      result.content = "Invalid Gemini streaming event.";
      result.error_code = agent::make_error_code(agent::llm_error_code::invalid_response);
      return false;
    }
    if (data.contains("error")) {
      result.error_code =
        classify_gemini_error(agent::make_error_code(agent::llm_error_code::api_error), data);
      result.content = gemini_error_message(result.error_code, data);
      return false;
    }

    llm_response chunk;
    if (data.contains("usageMetadata") && data["usageMetadata"].is_object()) {
      result.usage.prompt_tokens = data["usageMetadata"].value("promptTokenCount", result.usage.prompt_tokens);
      result.usage.completion_tokens = data["usageMetadata"].value("candidatesTokenCount", result.usage.completion_tokens);
      result.usage.total_tokens = data["usageMetadata"].value("totalTokenCount", result.usage.total_tokens);
    }
    if (data.contains("candidates") && data["candidates"].is_array()) {
      for (const auto& candidate : data["candidates"]) {
        append_candidate(chunk, candidate);
      }
    }
    if (!chunk.reasoning_summary.empty()) {
      result.reasoning_summary += chunk.reasoning_summary;
      for (const auto& [key, value] : chunk.reasoning_metadata) {
        result.reasoning_metadata[key] = value;
      }
      merge_reasoning_language_metadata(
        result.reasoning_metadata,
        request.language,
        reasoning_language_control,
        chunk.reasoning_summary);
      emitted_output = true;
      emit_stream_event(callbacks, {
        .type = llm_stream_event_type::reasoning_delta,
        .reasoning_delta = chunk.reasoning_summary,
        .reasoning_metadata = result.reasoning_metadata,
      });
    }
    if (!chunk.content.empty()) {
      result.content += chunk.content;
      emitted_output = true;
      emit_stream_event(callbacks, { .type = llm_stream_event_type::content_delta, .content_delta = chunk.content });
    }
    for (const auto& call : chunk.tool_calls) {
      result.tool_calls.push_back(call);
      emitted_output = true;
      emit_stream_event(callbacks, { .type = llm_stream_event_type::tool_call_done, .tool_call = call });
    }
    if (!chunk.finish_reason.empty()) {
      result.finish_reason = chunk.finish_reason;
      saw_done = true;
    }
    return true;
  };

  const http_request req {
    .method = "POST",
    .url = build_url(request, true),
    .headers = build_headers(),
    .body = build_payload(request).dump(),
    .timeout = config_.timeout,
    .timeouts = agent::llm_detail::make_stream_http_timeouts(config_),
  };
  const int max_retries = config_.max_retries < 0 ? 0 : config_.max_retries;
  const int base_backoff_ms = config_.retry_backoff_ms <= 0 ? 500 : config_.retry_backoff_ms;
  http_response response;
  for (int attempt = 0; attempt <= max_retries; ++attempt) {
    response = http_->send_stream(
      req,
      [&](std::string_view chunk) {
        return !stop_token.stop_requested() && parser.feed(chunk, process_event);
      },
      stop_token);
    if (!response.error_code || emitted_output || attempt >= max_retries) {
      break;
    }
    const auto body = json::parse(response.body, nullptr, false);
    const auto error_code = classify_gemini_error(
      response.error_code,
      body.is_discarded() ? json::object() : body);
    if (!agent::llm_detail::is_retryable_error(error_code)) {
      break;
    }
    if (!agent::llm_detail::wait_for_retry(
          stop_token,
          std::chrono::milliseconds(agent::llm_detail::compute_backoff_ms(
            attempt,
            base_backoff_ms)))) {
      result.error_code = agent::make_error_code(agent::llm_error_code::cancelled);
      break;
    }
    result = {};
    parser = {};
    timeout_guard = agent::llm_detail::stream_timeout_guard(config_.stream_timeouts);
    saw_event = false;
    saw_done = false;
  }
  if (!result.error_code) {
    parser.finish(process_event);
  }
  if (stop_token.stop_requested()) {
    result.error_code = agent::make_error_code(agent::llm_error_code::cancelled);
  }
  else if (response.error_code && !result.error_code) {
    const auto body = json::parse(response.body, nullptr, false);
    result.error_code =
      classify_gemini_error(response.error_code, body.is_discarded() ? json::object() : body);
    if (emitted_output && saw_event) {
      result.error_code.clear();
      result.metadata["ignored_stream_transport_error"] = response.error_code.message();
    }
    else {
      result.content = gemini_error_message(
        result.error_code,
        body.is_discarded() ? json::object() : body);
    }
  }
  else if (!result.error_code &&
           (!saw_event || (!saw_done && !(emitted_output && ignored_invalid_stream_event)))) {
    result.error_code = agent::make_error_code(agent::llm_error_code::invalid_response);
    result.content = "Gemini streaming response ended without a complete candidate.";
  }
  if (result.error_code) {
    emit_stream_event(callbacks, { .type = llm_stream_event_type::error, .response = result, .error_code = result.error_code, .message = result.content });
  }
  else {
    if (!result.reasoning_summary.empty()) {
      apply_reasoning_language_metadata(result, request.language, reasoning_language_control);
      emit_stream_event(callbacks, {
        .type = llm_stream_event_type::reasoning_done,
        .reasoning_summary = result.reasoning_summary,
        .reasoning_metadata = result.reasoning_metadata,
        .response = result,
      });
    }
    emit_stream_event(callbacks, { .type = llm_stream_event_type::done, .response = result });
  }
  return result;
}

WUWE_NAMESPACE_END
