#include <wuwe/agent/llm/anthropic_llm_client.h>

#include "llm_retry.hpp"

#include <wuwe/agent/llm/llm_error.h>
#include <wuwe/agent/llm/llm_provider_registry.h>
#include <wuwe/net/default_http_client.h>
#include <wuwe/net/net_errc.h>
#include <wuwe/net/sse_event_parser.h>

#include <map>
#include <utility>

WUWE_NAMESPACE_BEGIN

namespace {

using json = nlohmann::json;

json parse_json_object_or_default(const std::string& text) {
  const auto parsed = json::parse(text, nullptr, false);
  return parsed.is_object() ? parsed : json::object();
}

std::error_code classify_anthropic_error(const std::error_code& transport_or_http_error,
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
    const auto type = body["error"].value("type", std::string {});
    if (type == "authentication_error" || type == "permission_error") {
      return agent::make_error_code(agent::llm_error_code::authentication_failed);
    }
    if (type == "rate_limit_error") {
      return agent::make_error_code(agent::llm_error_code::rate_limited);
    }
    if (type == "not_found_error") {
      return agent::make_error_code(agent::llm_error_code::model_unavailable);
    }
    return agent::make_error_code(agent::llm_error_code::api_error);
  }
  return transport_or_http_error;
}

std::string anthropic_error_message(const std::error_code& error_code, const json& body) {
  if (body.is_object() && body.contains("error") && body["error"].is_object()) {
    const auto& error = body["error"];
    if (error.contains("message") && error["message"].is_string()) {
      return error["message"].get<std::string>();
    }
    const auto type = error.value("type", std::string {});
    if (!type.empty()) {
      return "Anthropic API error: " + type;
    }
  }
  if (error_code) {
    return "Anthropic streaming request failed: " + error_code.message();
  }
  return "Anthropic streaming request failed.";
}

void emit_stream_event(const llm_stream_callbacks& callbacks, llm_stream_event event) {
  if (callbacks.on_event) {
    callbacks.on_event(event);
  }
}

} // namespace

anthropic_llm_client::anthropic_llm_client(llm_client_config config)
    : config_(normalize_config(std::move(config))),
      http_(std::make_shared<default_http_client>()) {}

anthropic_llm_client::anthropic_llm_client(
  llm_client_config config,
  std::shared_ptr<http_client> http)
    : config_(normalize_config(std::move(config))),
      http_(std::move(http)) {
  if (!http_) {
    http_ = std::make_shared<default_http_client>();
  }
}

llm_client_config anthropic_llm_client::normalize_config(llm_client_config config) {
  if (auto normalized = normalize_llm_client_config("Anthropic", config)) {
    return std::move(*normalized);
  }
  return config;
}

json anthropic_llm_client::build_payload(const llm_request& request, bool stream) const {
  json messages = json::array();
  std::string system;

  for (const auto& msg : request.messages) {
    if (msg.role == "system") {
      if (!system.empty()) {
        system += "\n\n";
      }
      system += msg.content;
      continue;
    }

    json message;
    message["role"] = msg.role == "assistant" ? "assistant" : "user";

    if (msg.role == "tool") {
      message["content"] = json::array({
        {
          {"type", "tool_result"},
          {"tool_use_id", msg.tool_call_id.value_or("")},
          {"content", msg.content}
        }
      });
    }
    else if (!msg.tool_calls.empty()) {
      auto content = json::array();
      if (!msg.content.empty()) {
        content.push_back({{"type", "text"}, {"text", msg.content}});
      }
      for (const auto& call : msg.tool_calls) {
        content.push_back({
          {"type", "tool_use"},
          {"id", call.id},
          {"name", call.name},
          {"input", parse_json_object_or_default(call.arguments_json)}
        });
      }
      message["content"] = std::move(content);
    }
    else {
      message["content"] = msg.content;
    }
    messages.push_back(std::move(message));
  }

  json payload = {
    {"model", request.model.empty() ? config_.model : request.model},
    {"max_tokens", 4096},
    {"messages", std::move(messages)},
    {"temperature", request.temperature},
    {"stream", stream},
  };
  if (!system.empty()) {
    payload["system"] = system;
  }

  if (!request.tools.empty()) {
    auto tools = json::array();
    for (const auto& tool : request.tools) {
      tools.push_back({
        {"name", tool.name},
        {"description", tool.description},
        {"input_schema", parse_json_object_or_default(tool.parameters_json_schema)}
      });
    }
    payload["tools"] = std::move(tools);
  }

  if (request.tool_choice.has_value()) {
    switch (request.tool_choice->mode) {
      case llm_tool_choice_mode::none:
        payload["tool_choice"] = {{"type", "none"}};
        break;
      case llm_tool_choice_mode::required:
        payload["tool_choice"] = {{"type", "any"}};
        break;
      case llm_tool_choice_mode::named:
        payload["tool_choice"] = {{"type", "tool"}, {"name", request.tool_choice->name}};
        break;
      default:
        payload["tool_choice"] = {{"type", "auto"}};
        break;
    }
  }

  return payload;
}

std::vector<std::pair<std::string, std::string>> anthropic_llm_client::build_headers() const {
  std::vector<std::pair<std::string, std::string>> headers {
    {"Content-Type", "application/json"},
    {"anthropic-version", "2023-06-01"},
  };
  if (!config_.api_key.empty()) {
    headers.push_back({"x-api-key", config_.api_key});
  }
  return headers;
}

llm_response anthropic_llm_client::parse_response(const http_response& response) const {
  llm_response result;
  const auto data = json::parse(response.body, nullptr, false);

  if (response.error_code) {
    result.content = response.body;
    result.error_code = classify_anthropic_error(
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
      classify_anthropic_error(agent::make_error_code(agent::llm_error_code::api_error), data);
    return result;
  }

  result.finish_reason = data.value("stop_reason", std::string {});
  if (data.contains("usage") && data["usage"].is_object()) {
    result.usage.prompt_tokens = data["usage"].value("input_tokens", 0);
    result.usage.completion_tokens = data["usage"].value("output_tokens", 0);
    result.usage.total_tokens = result.usage.prompt_tokens + result.usage.completion_tokens;
  }

  if (!data.contains("content") || !data["content"].is_array()) {
    result.error_code = agent::make_error_code(agent::llm_error_code::invalid_response);
    result.content = response.body;
    return result;
  }

  for (const auto& block : data["content"]) {
    if (!block.is_object()) {
      continue;
    }
    const auto type = block.value("type", std::string {});
    if (type == "text") {
      result.content += block.value("text", std::string {});
    }
    else if (type == "tool_use") {
      result.tool_calls.push_back({
        .id = block.value("id", std::string {}),
        .name = block.value("name", std::string {}),
        .arguments_json = block.contains("input") ? block["input"].dump() : std::string("{}"),
      });
    }
  }

  return result;
}

llm_response anthropic_llm_client::complete(const llm_request& request) {
  return complete(request, {});
}

llm_response anthropic_llm_client::complete(const llm_request& request, std::stop_token stop_token) {
  if (stop_token.stop_requested()) {
    return { .error_code = agent::make_error_code(agent::llm_error_code::cancelled) };
  }
  if (config_.require_api_key && config_.api_key.empty()) {
    return { .error_code = agent::make_error_code(agent::llm_error_code::missing_api_key) };
  }

  const http_request req {
    .method = "POST",
    .url = config_.base_url + "/v1/messages",
    .headers = build_headers(),
    .body = build_payload(request, false).dump(),
    .timeout = config_.timeout,
  };
  const int max_retries = config_.max_retries < 0 ? 0 : config_.max_retries;
  const int base_backoff_ms = config_.retry_backoff_ms <= 0 ? 500 : config_.retry_backoff_ms;
  for (int attempt = 0; attempt <= max_retries; ++attempt) {
    if (stop_token.stop_requested()) {
      return { .error_code = agent::make_error_code(agent::llm_error_code::cancelled) };
    }
    auto result = parse_response(http_->send(req));
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

llm_response anthropic_llm_client::complete_stream(
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
  sse_event_parser parser;
  std::map<int, llm_tool_call> tool_calls;
  bool emitted_output = false;
  bool saw_event = false;
  bool saw_done = false;
  bool ignored_stream_transport_error = false;
  bool ignored_invalid_stream_event = false;

  const auto process_event = [&](const sse_event& event) {
    if (event.data.empty()) {
      return true;
    }
    saw_event = true;
    const auto data = json::parse(event.data, nullptr, false);
    if (data.is_discarded() || !data.is_object()) {
      if (emitted_output) {
        ignored_invalid_stream_event = true;
        result.metadata["ignored_invalid_stream_event"] = "true";
        return true;
      }
      result.content = "Invalid Anthropic streaming event.";
      result.error_code = agent::make_error_code(agent::llm_error_code::invalid_response);
      return false;
    }
    const auto type = data.value("type", std::string {});
    if (type == "error") {
      result.error_code =
        classify_anthropic_error(agent::make_error_code(agent::llm_error_code::api_error), data);
      result.content = anthropic_error_message(result.error_code, data);
      return false;
    }
    if (type == "message_start" && data.contains("message") && data["message"].is_object()) {
      const auto& message = data["message"];
      if (message.contains("usage") && message["usage"].is_object()) {
        result.usage.prompt_tokens = message["usage"].value("input_tokens", 0);
      }
    }
    else if (type == "content_block_start" && data.contains("content_block") &&
             data["content_block"].is_object()) {
      const int index = data.value("index", 0);
      const auto& block = data["content_block"];
      if (block.value("type", std::string {}) == "tool_use") {
        tool_calls[index] = {
          .id = block.value("id", std::string {}),
          .name = block.value("name", std::string {}),
          .arguments_json = "",
        };
      }
    }
    else if (type == "content_block_delta" && data.contains("delta") && data["delta"].is_object()) {
      const int index = data.value("index", 0);
      const auto& delta = data["delta"];
      if (delta.value("type", std::string {}) == "text_delta") {
        const auto text = delta.value("text", std::string {});
        result.content += text;
        emitted_output = true;
        emit_stream_event(callbacks, { .type = llm_stream_event_type::content_delta, .content_delta = text });
      }
      else if (delta.value("type", std::string {}) == "input_json_delta") {
        auto& call = tool_calls[index];
        const auto partial = delta.value("partial_json", std::string {});
        call.arguments_json += partial;
        emitted_output = true;
        emit_stream_event(callbacks, {
          .type = llm_stream_event_type::tool_call_delta,
          .tool_call_delta = llm_tool_call_delta {
            .index = index,
            .id = call.id,
            .arguments_delta = partial,
          },
        });
      }
    }
    else if (type == "content_block_stop") {
      const int index = data.value("index", 0);
      const auto it = tool_calls.find(index);
      if (it != tool_calls.end()) {
        result.tool_calls.push_back(it->second);
        emitted_output = true;
        emit_stream_event(callbacks, {
          .type = llm_stream_event_type::tool_call_done,
          .tool_call = it->second,
        });
      }
    }
    else if (type == "message_delta" && data.contains("delta") && data["delta"].is_object()) {
      result.finish_reason = data["delta"].value("stop_reason", result.finish_reason);
      if (data.contains("usage") && data["usage"].is_object()) {
        result.usage.completion_tokens = data["usage"].value("output_tokens", 0);
        result.usage.total_tokens = result.usage.prompt_tokens + result.usage.completion_tokens;
      }
    }
    else if (type == "message_stop") {
      saw_done = true;
      emit_stream_event(callbacks, { .type = llm_stream_event_type::done, .response = result });
    }
    return true;
  };

  const http_request req {
    .method = "POST",
    .url = config_.base_url + "/v1/messages",
    .headers = build_headers(),
    .body = build_payload(request, true).dump(),
    .timeout = config_.timeout,
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
    const auto error_code = classify_anthropic_error(
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
    tool_calls.clear();
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
      classify_anthropic_error(response.error_code, body.is_discarded() ? json::object() : body);
    if (emitted_output && saw_event) {
      result.error_code.clear();
      ignored_stream_transport_error = true;
      result.metadata["ignored_stream_transport_error"] = response.error_code.message();
    }
    else {
      result.content = anthropic_error_message(
        result.error_code,
        body.is_discarded() ? json::object() : body);
    }
  }
  else if (!result.error_code &&
           (!saw_event || (!saw_done && !(emitted_output && ignored_invalid_stream_event)))) {
    result.error_code = agent::make_error_code(agent::llm_error_code::invalid_response);
    result.content = "Anthropic streaming response ended without a complete message.";
  }
  if (result.error_code) {
    emit_stream_event(callbacks, {
      .type = llm_stream_event_type::error,
      .response = result,
      .error_code = result.error_code,
      .message = result.content,
    });
  }
  else if ((ignored_stream_transport_error || ignored_invalid_stream_event) && !saw_done) {
    emit_stream_event(callbacks, { .type = llm_stream_event_type::done, .response = result });
  }
  return result;
}

WUWE_NAMESPACE_END
