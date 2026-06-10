#include <wuwe/agent/llm/openrouter_llm_client.h>

#include <wuwe/agent/llm/llm_error.h>
#include <wuwe/net/net_errc.h>
#include <wuwe/net/default_http_client.h>

#include <algorithm>
#include <chrono>
#include <system_error>
#include <thread>

WUWE_NAMESPACE_BEGIN

namespace {

json parse_json_object_or_default(const std::string& text, const json& fallback) {
  const auto parsed = json::parse(text, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_object()) {
    return fallback;
  }
  return parsed;
}

json build_tool_choice_json(const llm_tool_choice& tool_choice) {
  switch (tool_choice.mode) {
    case llm_tool_choice_mode::auto_:
      return "auto";
    case llm_tool_choice_mode::none:
      return "none";
    case llm_tool_choice_mode::required:
      return "required";
    case llm_tool_choice_mode::named:
      return {
        {"type", "function"},
        {"function", {{"name", tool_choice.name}}}
      };
    default:
      return "auto";
  }
}

bool is_retryable_error(const std::error_code& ec) {
  return ec == agent::llm_error_code::rate_limited || ec == agent::llm_error_code::timeout ||
         ec == net_errc::rate_limited || ec == net_errc::timeout ||
         ec == net_errc::connection_failed || ec == net_errc::transport_failed ||
         ec == net_errc::server_error || ec == net_errc::service_unavailable;
}

int compute_backoff_ms(int attempt, int base_backoff_ms) {
  constexpr int max_power = 6;
  const int clamped_attempt = attempt < max_power ? attempt : max_power;
  return base_backoff_ms * (1 << clamped_attempt);
}

bool wait_for_retry(std::stop_token stop_token, std::chrono::milliseconds duration) {
  constexpr auto poll_interval = std::chrono::milliseconds(50);
  auto remaining = duration;
  while (remaining.count() > 0) {
    if (stop_token.stop_requested()) {
      return false;
    }
    const auto sleep_time = (std::min)(remaining, poll_interval);
    std::this_thread::sleep_for(sleep_time);
    remaining -= sleep_time;
  }
  return !stop_token.stop_requested();
}

std::error_code classify_openai_error(const std::error_code& transport_or_http_error,
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
    const auto& error = body["error"];
    const auto code = error.value("code", std::string {});
    const auto type = error.value("type", std::string {});

    if (code == "invalid_api_key" || type == "invalid_api_key" ||
        code == "unauthorized" || type == "authentication_error") {
      return agent::make_error_code(agent::llm_error_code::authentication_failed);
    }
    if (code == "rate_limit_exceeded" || type == "rate_limit_exceeded" ||
        code == "rate_limited") {
      return agent::make_error_code(agent::llm_error_code::rate_limited);
    }
    if (code == "model_not_found" || type == "model_not_found" ||
        code == "model_unavailable") {
      return agent::make_error_code(agent::llm_error_code::model_unavailable);
    }

    return agent::make_error_code(agent::llm_error_code::api_error);
  }

  return transport_or_http_error;
}

} // namespace

openrouter_llm_client::openrouter_llm_client(llm_client_config config)
    : config_(std::move(config)), http_(std::make_shared<default_http_client>()) {}

llm_response openrouter_llm_client::complete(const llm_request& request) {
  return complete(request, {});
}

llm_response openrouter_llm_client::complete(const llm_request& request, std::stop_token stop_token) {
  if (stop_token.stop_requested()) {
    return { .error_code = agent::make_error_code(agent::llm_error_code::cancelled) };
  }
  if (config_.require_api_key && config_.api_key.empty()) {
    return { .error_code = agent::make_error_code(agent::llm_error_code::missing_api_key) };
  }

  const auto payload = build_openai_payload(request);

  std::vector<std::pair<std::string, std::string>> headers {
    {"Content-Type", "application/json"},
  };
  if (!config_.api_key.empty()) {
    headers.push_back({"Authorization", "Bearer " + config_.api_key});
  }
  if (config_.base_url.find("openrouter.ai") != std::string::npos) {
    if (!config_.referer_url.empty()) {
      headers.push_back({"HTTP-Referer", config_.referer_url});
    }
    if (!config_.app_title.empty()) {
      headers.push_back({"X-Title", config_.app_title});
    }
  }

  const http_request req {
    .method = "POST",
    .url = config_.base_url + "/v1/chat/completions",
    .headers = std::move(headers),
    .body = payload.dump(),
    .timeout = config_.timeout,
  };

  const int max_retries = config_.max_retries < 0 ? 0 : config_.max_retries;
  const int base_backoff_ms = config_.retry_backoff_ms <= 0 ? 500 : config_.retry_backoff_ms;

  for (int attempt = 0; attempt <= max_retries; ++attempt) {
    if (stop_token.stop_requested()) {
      return { .error_code = agent::make_error_code(agent::llm_error_code::cancelled) };
    }

    const auto response = http_->send(req);
    llm_response parsed = parse_openai_response(response);
    if (stop_token.stop_requested()) {
      return { .error_code = agent::make_error_code(agent::llm_error_code::cancelled) };
    }
    if (!parsed.error_code) {
      return parsed;
    }
    if (attempt >= max_retries || !is_retryable_error(parsed.error_code)) {
      return parsed;
    }
    if (!wait_for_retry(
          stop_token,
          std::chrono::milliseconds(compute_backoff_ms(attempt, base_backoff_ms)))) {
      return { .error_code = agent::make_error_code(agent::llm_error_code::cancelled) };
    }
  }

  llm_response fallback;
  fallback.error_code = std::make_error_code(std::errc::io_error);
  return fallback;
}

json openrouter_llm_client::build_openai_payload(const llm_request& request) const {
  auto messages = json::array();

  for (const auto& msg : request.messages) {
    json message = {
      {"role", msg.role},
      {"content", msg.content}
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
            {"arguments", call.arguments_json}
          }}
        });
      }
      message["tool_calls"] = std::move(tool_calls);
    }
    messages.push_back(std::move(message));
  }

  nlohmann::json payload = {
    { "model", request.model.empty() ? config_.model : request.model },
    { "messages", messages },
    { "temperature", request.temperature },
  };

  if (request.response_format.has_value()) {
    if (*request.response_format == "json_object") {
      payload["response_format"] = { { "type", "json_object" } };
    }
  }

  if (!request.tools.empty()) {
    auto tools = json::array();
    for (const auto& tool : request.tools) {
      tools.push_back({
        {"type", "function"},
        {"function", {
          {"name", tool.name},
          {"description", tool.description},
          {"parameters", parse_json_object_or_default(tool.parameters_json_schema, json::object())}
        }}
      });
    }
    payload["tools"] = std::move(tools);
  }

  if (request.tool_choice.has_value()) {
    payload["tool_choice"] = build_tool_choice_json(*request.tool_choice);
  }

  return payload;
}

llm_response openrouter_llm_client::parse_openai_response(const http_response& response) const {
  llm_response result;

  if (response.error_code) {
    result.content = response.body;
    const auto body = json::parse(response.body, nullptr, false);
    result.error_code =
      classify_openai_error(response.error_code, body.is_discarded() ? json::object() : body);
    return result;
  }

  const auto data = json::parse(response.body, nullptr, false);
  if (data.is_discarded()) {
    result.error_code = agent::make_error_code(agent::llm_error_code::invalid_response);
    result.content = response.body;
    return result;
  }

  if (data.contains("error") && data["error"].is_object()) {
    result.error_code = classify_openai_error(
      agent::make_error_code(agent::llm_error_code::api_error),
      data);
    result.content = response.body;
    return result;
  }

  if (!data.contains("choices") || !data["choices"].is_array() || data["choices"].empty() ||
      !data["choices"][0].contains("message")) {
    result.error_code = agent::make_error_code(agent::llm_error_code::invalid_response);
    result.content = response.body;
    return result;
  }

  const auto& choice = data["choices"][0];
  const auto& message = choice["message"];

  if (choice.contains("finish_reason") && choice["finish_reason"].is_string()) {
    result.finish_reason = choice["finish_reason"].get<std::string>();
  }

  if (message.contains("content") && message["content"].is_string()) {
    result.content = message["content"].get<std::string>();
  }

  if (message.contains("tool_calls") && message["tool_calls"].is_array()) {
    for (const auto& call : message["tool_calls"]) {
      if (!call.is_object() || !call.contains("function") || !call["function"].is_object()) {
        continue;
      }
      const auto& function = call["function"];
      llm_tool_call tool_call;
      if (call.contains("id") && call["id"].is_string()) {
        tool_call.id = call["id"].get<std::string>();
      }
      if (function.contains("name") && function["name"].is_string()) {
        tool_call.name = function["name"].get<std::string>();
      }
      if (function.contains("arguments") && function["arguments"].is_string()) {
        tool_call.arguments_json = function["arguments"].get<std::string>();
      }
      result.tool_calls.push_back(std::move(tool_call));
    }
  }

  if (data.contains("usage") && data["usage"].is_object()) {
    const auto& usage = data["usage"];
    result.usage.prompt_tokens = usage.value("prompt_tokens", 0);
    result.usage.completion_tokens = usage.value("completion_tokens", 0);
    result.usage.total_tokens = usage.value("total_tokens", 0);
  }

  return result;
}

WUWE_NAMESPACE_END
