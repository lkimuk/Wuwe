#include <wuwe/agent/llm/openrouter_llm_client.h>

#include <wuwe/net/default_http_client.h>

#include <system_error>

WUWE_NAMESPACE_BEGIN

openrouter_llm_client::openrouter_llm_client(llm_client_config config)
    : config_(std::move(config)), http_(std::make_shared<default_http_client>()) {}

llm_response openrouter_llm_client::complete(const llm_request& request) {
  auto payload = build_openai_payload(request);

  http_request req {
    .method = "POST",
    .url = config_.base_url + "/v1/chat/completions",
    .headers = { { "Authorization", "Bearer " + config_.api_key },
      { "Content-Type", "application/json" } },
    .body = payload.dump(),
    .timeout = config_.timeout,
  };

  const auto response = http_->send(req);
  return parse_openai_response(response);
}

json openrouter_llm_client::build_openai_payload(const llm_request& request) const {
  auto messages = json::array();

  for (const auto& msg : request.messages) {
    messages.push_back({ { "role", msg.role }, { "content", msg.content } });
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
      !data["choices"][0].contains("message") ||
      !data["choices"][0]["message"].contains("content")) {
    result.error_code = std::make_error_code(std::errc::protocol_error);
    return result;
  }

  result.content = data["choices"][0]["message"]["content"].get<std::string>();

  if (data.contains("usage") && data["usage"].is_object()) {
    const auto& usage = data["usage"];
    result.usage.prompt_tokens = usage.value("prompt_tokens", 0);
    result.usage.completion_tokens = usage.value("completion_tokens", 0);
    result.usage.total_tokens = usage.value("total_tokens", 0);
  }

  return result;
}

WUWE_NAMESPACE_END
