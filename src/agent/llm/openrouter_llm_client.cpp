#include <wuwe/agent/llm/openrouter_llm_client.h>

#include <wuwe/net/net_errc.h>
#include <wuwe/net/default_http_client.h>

#include <chrono>
#include <thread>
#include <system_error>

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
  return ec == net_errc::rate_limited || ec == net_errc::timeout ||
         ec == net_errc::connection_failed || ec == net_errc::transport_failed ||
         ec == net_errc::server_error || ec == net_errc::service_unavailable;
}

int compute_backoff_ms(int attempt, int base_backoff_ms) {
  constexpr int max_power = 6;
  const int clamped_attempt = attempt < max_power ? attempt : max_power;
  return base_backoff_ms * (1 << clamped_attempt);
}

} // namespace

openrouter_llm_client::openrouter_llm_client(llm_client_config config)
    : config_(std::move(config)), http_(std::make_shared<default_http_client>()) {}

llm_response openrouter_llm_client::complete(const llm_request& request) {
  const auto payload = build_openai_payload(request);

  const http_request req {
    .method = "POST",
    .url = config_.base_url + "/v1/chat/completions",
    .headers = {{"Authorization", "Bearer " + config_.api_key},
      {"Content-Type", "application/json"}},
    .body = payload.dump(),
    .timeout = config_.timeout,
  };

  const int max_retries = config_.max_retries < 0 ? 0 : config_.max_retries;
  const int base_backoff_ms = config_.retry_backoff_ms <= 0 ? 500 : config_.retry_backoff_ms;

  for (int attempt = 0; attempt <= max_retries; ++attempt) {
    const auto response = http_->send(req);
    llm_response parsed = parse_openai_response(response);
    if (!parsed.error_code) {
      return parsed;
    }
    if (attempt >= max_retries || !is_retryable_error(parsed.error_code)) {
      return parsed;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(compute_backoff_ms(attempt, base_backoff_ms)));
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
    result.error_code = response.error_code;
    return result;
  }

  const auto data = json::parse(response.body, nullptr, false);
  if (data.is_discarded()) {
    result.error_code = std::make_error_code(std::errc::protocol_error);
    return result;
  }

  if (!data.contains("choices") || !data["choices"].is_array() || data["choices"].empty() ||
      !data["choices"][0].contains("message")) {
    result.error_code = std::make_error_code(std::errc::protocol_error);
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
