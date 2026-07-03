#include <wuwe/agent/llm/ollama_llm_client.h>

#include "llm_retry.hpp"
#include "llm_stream_timeouts.hpp"

#include <wuwe/agent/llm/llm_error.h>
#include <wuwe/agent/llm/llm_provider_registry.h>
#include <wuwe/net/default_http_client.h>
#include <wuwe/net/net_errc.h>

#include <utility>

WUWE_NAMESPACE_BEGIN

namespace {

using json = nlohmann::json;

json parse_json_object_or_default(const std::string& text) {
  const auto parsed = json::parse(text, nullptr, false);
  return parsed.is_object() ? parsed : json::object();
}

std::error_code classify_ollama_error(const std::error_code& transport_or_http_error,
  const json& body) {
  if (transport_or_http_error == net_errc::unauthorized ||
      transport_or_http_error == net_errc::forbidden) {
    return agent::make_error_code(agent::llm_error_code::authentication_failed);
  }
  if (transport_or_http_error == net_errc::not_found) {
    return agent::make_error_code(agent::llm_error_code::model_unavailable);
  }
  if (transport_or_http_error == net_errc::rate_limited) {
    return agent::make_error_code(agent::llm_error_code::rate_limited);
  }
  if (transport_or_http_error == net_errc::timeout) {
    return agent::make_error_code(agent::llm_error_code::timeout);
  }
  if (body.is_object() && body.contains("error")) {
    return agent::make_error_code(agent::llm_error_code::api_error);
  }
  return transport_or_http_error;
}

std::string ollama_error_message(const std::error_code& error_code, const json& body) {
  if (body.is_object() && body.contains("error")) {
    if (body["error"].is_string()) {
      return body["error"].get<std::string>();
    }
    if (body["error"].is_object()) {
      const auto& error = body["error"];
      if (error.contains("message") && error["message"].is_string()) {
        return error["message"].get<std::string>();
      }
    }
    return "Ollama API error.";
  }
  if (error_code) {
    return "Ollama streaming request failed: " + error_code.message();
  }
  return "Ollama streaming request failed.";
}

void append_tool_calls(llm_response& result, const json& message) {
  if (!message.contains("tool_calls") || !message["tool_calls"].is_array()) {
    return;
  }
  for (const auto& call : message["tool_calls"]) {
    if (!call.is_object() || !call.contains("function") || !call["function"].is_object()) {
      continue;
    }
    const auto& function = call["function"];
    result.tool_calls.push_back({
      .id = call.value("id", std::string {}),
      .name = function.value("name", std::string {}),
      .arguments_json = function.contains("arguments")
                          ? (function["arguments"].is_string()
                              ? function["arguments"].get<std::string>()
                              : function["arguments"].dump())
                          : std::string("{}"),
    });
  }
}

std::string message_reasoning_text(const json& message) {
  if (!message.is_object()) {
    return {};
  }
  for (const auto* key : { "thinking", "reasoning", "reasoning_content" }) {
    const auto it = message.find(key);
    if (it != message.end() && it->is_string()) {
      return it->get<std::string>();
    }
  }
  return {};
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

ollama_llm_client::ollama_llm_client(llm_client_config config)
    : config_(normalize_config(std::move(config))),
      http_(std::make_shared<default_http_client>()) {}

ollama_llm_client::ollama_llm_client(
  llm_client_config config,
  std::shared_ptr<http_client> http)
    : config_(normalize_config(std::move(config))),
      http_(std::move(http)) {
  if (!http_) {
    http_ = std::make_shared<default_http_client>();
  }
}

llm_client_config ollama_llm_client::normalize_config(llm_client_config config) {
  if (auto normalized = normalize_llm_client_config("Ollama", config)) {
    return std::move(*normalized);
  }
  return config;
}

json ollama_llm_client::build_payload(const llm_request& request, bool stream) const {
  auto messages = json::array();
  for (const auto& msg : request.messages) {
    json message = {
      {"role", msg.role == "tool" ? "tool" : msg.role},
      {"content", msg.content},
    };
    if (msg.name.has_value()) {
      message["name"] = *msg.name;
    }
    if (msg.tool_call_id.has_value()) {
      message["tool_call_id"] = *msg.tool_call_id;
    }
    if (!msg.tool_calls.empty()) {
      auto tool_calls = json::array();
      for (const auto& call : msg.tool_calls) {
        tool_calls.push_back({
          {"id", call.id},
          {"type", "function"},
          {"function", {
            {"name", call.name},
            {"arguments", parse_json_object_or_default(call.arguments_json)}
          }}
        });
      }
      message["tool_calls"] = std::move(tool_calls);
    }
    messages.push_back(std::move(message));
  }

  json payload = {
    {"model", request.model.empty() ? config_.model : request.model},
    {"messages", std::move(messages)},
    {"stream", stream},
    {"options", {{"temperature", request.temperature}}},
  };
  if (request.response_format == "json_object") {
    payload["format"] = "json";
  }
  if (!request.tools.empty()) {
    auto tools = json::array();
    for (const auto& tool : request.tools) {
      tools.push_back({
        {"type", "function"},
        {"function", {
          {"name", tool.name},
          {"description", tool.description},
          {"parameters", parse_json_object_or_default(tool.parameters_json_schema)}
        }}
      });
    }
    payload["tools"] = std::move(tools);
  }
  return payload;
}

std::vector<std::pair<std::string, std::string>> ollama_llm_client::build_headers() const {
  std::vector<std::pair<std::string, std::string>> headers {
    {"Content-Type", "application/json"},
  };
  if (!config_.api_key.empty()) {
    headers.push_back({"Authorization", "Bearer " + config_.api_key});
  }
  return headers;
}

llm_response ollama_llm_client::parse_response(const http_response& response) const {
  llm_response result;
  const auto data = json::parse(response.body, nullptr, false);
  if (response.error_code) {
    result.content = response.body;
    result.error_code =
      classify_ollama_error(response.error_code, data.is_discarded() ? json::object() : data);
    return result;
  }
  if (data.is_discarded() || !data.is_object()) {
    result.content = response.body;
    result.error_code = agent::make_error_code(agent::llm_error_code::invalid_response);
    return result;
  }
  if (data.contains("error")) {
    result.content = data["error"].is_string() ? data["error"].get<std::string>() : response.body;
    result.error_code =
      classify_ollama_error(agent::make_error_code(agent::llm_error_code::api_error), data);
    return result;
  }
  if (!data.contains("message") || !data["message"].is_object()) {
    result.content = response.body;
    result.error_code = agent::make_error_code(agent::llm_error_code::invalid_response);
    return result;
  }
  const auto& message = data["message"];
  result.content = message.value("content", std::string {});
  result.reasoning_summary = message_reasoning_text(message);
  append_tool_calls(result, message);
  result.finish_reason = data.value("done_reason", std::string {});
  result.usage.prompt_tokens = data.value("prompt_eval_count", 0);
  result.usage.completion_tokens = data.value("eval_count", 0);
  result.usage.total_tokens = result.usage.prompt_tokens + result.usage.completion_tokens;
  return result;
}

llm_response ollama_llm_client::complete(const llm_request& request) {
  return complete(request, {});
}

llm_response ollama_llm_client::complete(const llm_request& request, std::stop_token stop_token) {
  if (stop_token.stop_requested()) {
    return { .error_code = agent::make_error_code(agent::llm_error_code::cancelled) };
  }
  const http_request req {
    .method = "POST",
    .url = config_.base_url + "/api/chat",
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

llm_response ollama_llm_client::complete_stream(
  const llm_request& request,
  const llm_stream_callbacks& callbacks,
  std::stop_token stop_token) {
  if (stop_token.stop_requested()) {
    return { .error_code = agent::make_error_code(agent::llm_error_code::cancelled) };
  }

  llm_response result;
  std::string buffer;
  bool emitted_output = false;
  bool saw_event = false;
  bool saw_done = false;
  bool ignored_stream_transport_error = false;
  bool ignored_invalid_stream_event = false;
  agent::llm_detail::stream_timeout_guard timeout_guard(config_.stream_timeouts);
  const auto fail_stream_timeout =
    [&](const agent::llm_detail::stream_timeout& timeout) {
      result.metadata["timeout_phase"] = timeout.phase;
      result.metadata["timeout_ms"] = std::to_string(timeout.timeout_ms);
      result.content = "Ollama streaming " + timeout.phase + " timeout.";
      result.error_code = agent::make_error_code(agent::llm_error_code::timeout);
      return false;
    };
  const auto process_line = [&](std::string line) {
    if (line.empty()) {
      return true;
    }
    if (const auto timeout = timeout_guard.check_before_event()) {
      return fail_stream_timeout(*timeout);
    }
    timeout_guard.mark_event();
    saw_event = true;
    const auto data = json::parse(line, nullptr, false);
    if (data.is_discarded() || !data.is_object()) {
      if (emitted_output) {
        ignored_invalid_stream_event = true;
        result.metadata["ignored_invalid_stream_event"] = "true";
        return true;
      }
      result.content = "Invalid Ollama streaming event.";
      result.error_code = agent::make_error_code(agent::llm_error_code::invalid_response);
      return false;
    }
    if (data.contains("error")) {
      result.error_code =
        classify_ollama_error(agent::make_error_code(agent::llm_error_code::api_error), data);
      result.content = ollama_error_message(result.error_code, data);
      return false;
    }
    if (data.contains("message") && data["message"].is_object()) {
      const auto& message = data["message"];
      const auto reasoning_delta = message_reasoning_text(message);
      if (!reasoning_delta.empty()) {
        result.reasoning_summary += reasoning_delta;
        emitted_output = true;
        emit_stream_event(callbacks, {
          .type = llm_stream_event_type::reasoning_delta,
          .reasoning_delta = reasoning_delta,
          .reasoning_metadata = result.reasoning_metadata,
        });
      }
      const auto delta = message.value("content", std::string {});
      if (!delta.empty()) {
        result.content += delta;
        emitted_output = true;
        emit_stream_event(callbacks, {
          .type = llm_stream_event_type::content_delta,
          .content_delta = delta,
        });
      }
      const auto before = result.tool_calls.size();
      append_tool_calls(result, message);
      for (std::size_t i = before; i < result.tool_calls.size(); ++i) {
        emitted_output = true;
        emit_stream_event(callbacks, {
          .type = llm_stream_event_type::tool_call_done,
          .tool_call = result.tool_calls[i],
        });
      }
    }
    if (data.value("done", false)) {
      saw_done = true;
      result.finish_reason = data.value("done_reason", std::string {});
      result.usage.prompt_tokens = data.value("prompt_eval_count", 0);
      result.usage.completion_tokens = data.value("eval_count", 0);
      result.usage.total_tokens = result.usage.prompt_tokens + result.usage.completion_tokens;
      if (!result.reasoning_summary.empty()) {
        emit_stream_event(callbacks, {
          .type = llm_stream_event_type::reasoning_done,
          .reasoning_summary = result.reasoning_summary,
          .reasoning_metadata = result.reasoning_metadata,
          .response = result,
        });
      }
      emit_stream_event(callbacks, { .type = llm_stream_event_type::done, .response = result });
    }
    return true;
  };

  const http_request req {
    .method = "POST",
    .url = config_.base_url + "/api/chat",
    .headers = build_headers(),
    .body = build_payload(request, true).dump(),
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
        if (stop_token.stop_requested()) {
          return false;
        }
        buffer.append(chunk.data(), chunk.size());
        for (;;) {
          const auto pos = buffer.find('\n');
          if (pos == std::string::npos) {
            return true;
          }
          auto line = buffer.substr(0, pos);
          if (!line.empty() && line.back() == '\r') {
            line.pop_back();
          }
          buffer.erase(0, pos + 1);
          if (!process_line(std::move(line))) {
            return false;
          }
        }
      },
      stop_token);
    if (!response.error_code || emitted_output || attempt >= max_retries) {
      break;
    }
    const auto body = json::parse(response.body, nullptr, false);
    const auto error_code =
      classify_ollama_error(response.error_code, body.is_discarded() ? json::object() : body);
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
    buffer.clear();
    timeout_guard = agent::llm_detail::stream_timeout_guard(config_.stream_timeouts);
    saw_event = false;
    saw_done = false;
  }
  if (!buffer.empty() && !result.error_code) {
    process_line(std::move(buffer));
  }
  if (stop_token.stop_requested()) {
    result.error_code = agent::make_error_code(agent::llm_error_code::cancelled);
  }
  else if (response.error_code && !result.error_code) {
    const auto body = json::parse(response.body, nullptr, false);
    result.error_code =
      classify_ollama_error(response.error_code, body.is_discarded() ? json::object() : body);
    if (emitted_output && saw_event) {
      result.error_code.clear();
      ignored_stream_transport_error = true;
      result.metadata["ignored_stream_transport_error"] = response.error_code.message();
    }
    else {
      result.content = ollama_error_message(
        result.error_code,
        body.is_discarded() ? json::object() : body);
    }
  }
  else if (!result.error_code &&
           (!saw_event || (!saw_done && !(emitted_output && ignored_invalid_stream_event)))) {
    result.error_code = agent::make_error_code(agent::llm_error_code::invalid_response);
    result.content = "Ollama streaming response ended without a complete message.";
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
    if (!result.reasoning_summary.empty()) {
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
