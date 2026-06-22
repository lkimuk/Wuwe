#include <wuwe/agent/llm/openai_compatible_llm_client.h>

#include "llm_retry.hpp"
#include "llm_stream_timeouts.hpp"

#include <wuwe/agent/llm/llm_error.h>
#include <wuwe/net/net_errc.h>
#include <wuwe/net/default_http_client.h>
#include <wuwe/net/sse_event_parser.h>

#include <chrono>
#include <map>
#include <sstream>
#include <string>
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

void emit_stream_event(const llm_stream_callbacks& callbacks, llm_stream_event event) {
  if (callbacks.on_event) {
    callbacks.on_event(event);
  }
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

std::string openai_error_message(const std::error_code& error_code, const json& body) {
  if (body.is_object() && body.contains("error") && body["error"].is_object()) {
    const auto& error = body["error"];
    if (error.contains("message") && error["message"].is_string()) {
      return error["message"].get<std::string>();
    }
    const auto type = error.value("type", std::string {});
    const auto code = error.value("code", std::string {});
    if (!type.empty() || !code.empty()) {
      std::ostringstream message;
      message << "OpenAI-compatible API error";
      if (!type.empty()) {
        message << " (" << type << ")";
      }
      if (!code.empty()) {
        message << ": " << code;
      }
      return message.str();
    }
  }

  if (error_code) {
    return "OpenAI-compatible streaming request failed: " + error_code.message();
  }
  return "OpenAI-compatible streaming request failed.";
}

std::string invalid_stream_event_message() {
  return "Invalid OpenAI-compatible streaming event.";
}

std::string build_chat_completions_url(const llm_client_config& config) {
  const auto path = config.chat_completions_path.empty()
                      ? std::string { "/v1/chat/completions" }
                      : config.chat_completions_path;
  if (config.base_url.empty()) {
    return path.front() == '/' ? path : "/" + path;
  }
  const bool base_has_slash = config.base_url.back() == '/';
  const bool path_has_slash = path.front() == '/';
  if (base_has_slash && path_has_slash) {
    return config.base_url + path.substr(1);
  }
  if (!base_has_slash && !path_has_slash) {
    return config.base_url + "/" + path;
  }
  if (path_has_slash) {
    return config.base_url + path;
  }
  return config.base_url + path;
}

} // namespace

openai_compatible_llm_client::openai_compatible_llm_client(llm_client_config config)
    : config_(normalize_config(std::move(config))),
      http_(std::make_shared<default_http_client>()) {}

openai_compatible_llm_client::openai_compatible_llm_client(
  llm_client_config config,
  std::shared_ptr<http_client> http)
    : config_(normalize_config(std::move(config))),
      http_(std::move(http)) {
  if (!http_) {
    http_ = std::make_shared<default_http_client>();
  }
}

llm_client_config openai_compatible_llm_client::normalize_config(llm_client_config config) {
  if (config.api_key.empty() && config.load_api_key_from_environment) {
    config.api_key = llm_client_config::load_api_key_from_env();
  }
  return config;
}

llm_response openai_compatible_llm_client::complete(const llm_request& request) {
  return complete(request, {});
}

llm_response openai_compatible_llm_client::complete(const llm_request& request, std::stop_token stop_token) {
  if (stop_token.stop_requested()) {
    return { .error_code = agent::make_error_code(agent::llm_error_code::cancelled) };
  }
  if (config_.require_api_key && config_.api_key.empty()) {
    return { .error_code = agent::make_error_code(agent::llm_error_code::missing_api_key) };
  }

  const auto payload = build_openai_payload(request);

  const http_request req {
    .method = "POST",
    .url = build_chat_completions_url(config_),
    .headers = build_headers(),
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
    if (attempt >= max_retries || !agent::llm_detail::is_retryable_error(parsed.error_code)) {
      return parsed;
    }
    if (!agent::llm_detail::wait_for_retry(
          stop_token,
          std::chrono::milliseconds(agent::llm_detail::compute_backoff_ms(
            attempt,
            base_backoff_ms)))) {
      return { .error_code = agent::make_error_code(agent::llm_error_code::cancelled) };
    }
  }

  llm_response fallback;
  fallback.error_code = std::make_error_code(std::errc::io_error);
  return fallback;
}

llm_response openai_compatible_llm_client::complete_stream(
  const llm_request& request,
  const llm_stream_callbacks& callbacks,
  std::stop_token stop_token) {
  if (stop_token.stop_requested()) {
    auto response = llm_response {
      .error_code = agent::make_error_code(agent::llm_error_code::cancelled),
    };
    emit_stream_event(callbacks, {
      .type = llm_stream_event_type::error,
      .response = response,
      .error_code = response.error_code,
    });
    return response;
  }
  if (config_.require_api_key && config_.api_key.empty()) {
    auto response = llm_response {
      .error_code = agent::make_error_code(agent::llm_error_code::missing_api_key),
    };
    emit_stream_event(callbacks, {
      .type = llm_stream_event_type::error,
      .response = response,
      .error_code = response.error_code,
    });
    return response;
  }

  auto payload = build_openai_payload(request);
  payload["stream"] = true;
  payload["stream_options"] = { { "include_usage", true } };

  const http_request req {
    .method = "POST",
    .url = build_chat_completions_url(config_),
    .headers = build_headers(),
    .body = payload.dump(),
    .timeout = config_.timeout,
    .timeouts = agent::llm_detail::make_stream_http_timeouts(config_),
  };

  const int max_retries = config_.max_retries < 0 ? 0 : config_.max_retries;
  const int base_backoff_ms = config_.retry_backoff_ms <= 0 ? 500 : config_.retry_backoff_ms;

  for (int attempt = 0; attempt <= max_retries; ++attempt) {
    if (stop_token.stop_requested()) {
      auto response = llm_response {
        .error_code = agent::make_error_code(agent::llm_error_code::cancelled),
      };
      emit_stream_event(callbacks, {
        .type = llm_stream_event_type::error,
        .response = response,
        .error_code = response.error_code,
      });
      return response;
    }

    llm_response result;
    sse_event_parser parser;
    std::map<int, llm_tool_call> tool_calls;
    bool emitted_output = false;
    bool saw_sse_event = false;
    bool saw_done = false;
    bool stream_parse_failed = false;
    agent::llm_detail::stream_timeout_guard timeout_guard(config_.stream_timeouts);

    const auto fail_stream = [&](std::error_code error_code, std::string content) {
      result.error_code = error_code;
      result.content = std::move(content);
      stream_parse_failed = true;
      emit_stream_event(callbacks, {
        .type = llm_stream_event_type::error,
        .response = result,
        .error_code = result.error_code,
        .message = result.content,
      });
      return false;
    };
    const auto fail_stream_timeout =
      [&](std::string phase, int timeout_ms) {
        result.metadata["timeout_phase"] = phase;
        result.metadata["timeout_ms"] = std::to_string(timeout_ms);
        return fail_stream(
          agent::make_error_code(agent::llm_error_code::timeout),
          "OpenAI-compatible streaming " + phase + " timeout.");
      };

    const auto process_event = [&](const sse_event& event) {
      if (const auto timeout = timeout_guard.check_before_event()) {
        return fail_stream_timeout(timeout->phase, timeout->timeout_ms);
      }
      timeout_guard.mark_event();
      saw_sse_event = true;
      if (event.data == "[DONE]") {
        saw_done = true;
        return true;
      }

      const auto data = json::parse(event.data, nullptr, false);
      if (data.is_discarded() || !data.is_object()) {
        if (emitted_output) {
          result.metadata["ignored_invalid_stream_event"] = "true";
          return true;
        }
        return fail_stream(
          agent::make_error_code(agent::llm_error_code::invalid_response),
          invalid_stream_event_message());
      }

      if (data.contains("error") && data["error"].is_object()) {
        return fail_stream(
          classify_openai_error(
            agent::make_error_code(agent::llm_error_code::api_error),
            data),
          openai_error_message(
            agent::make_error_code(agent::llm_error_code::api_error),
            data));
      }

      if (data.contains("usage") && data["usage"].is_object()) {
        const auto& usage = data["usage"];
        result.usage.prompt_tokens = usage.value("prompt_tokens", 0);
        result.usage.completion_tokens = usage.value("completion_tokens", 0);
        result.usage.total_tokens = usage.value("total_tokens", 0);
      }

      if (!data.contains("choices") || !data["choices"].is_array()) {
        return true;
      }

      for (const auto& choice : data["choices"]) {
        if (!choice.is_object()) {
          continue;
        }
        if (choice.contains("finish_reason") && choice["finish_reason"].is_string()) {
          result.finish_reason = choice["finish_reason"].get<std::string>();
        }
        if (!choice.contains("delta") || !choice["delta"].is_object()) {
          continue;
        }

        const auto& delta = choice["delta"];
        if (delta.contains("content") && delta["content"].is_string()) {
          const auto content_delta = delta["content"].get<std::string>();
          if (!content_delta.empty()) {
            result.content += content_delta;
            emitted_output = true;
            emit_stream_event(callbacks, {
              .type = llm_stream_event_type::content_delta,
              .content_delta = content_delta,
            });
          }
        }

        if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
          for (const auto& call_delta_json : delta["tool_calls"]) {
            if (!call_delta_json.is_object()) {
              continue;
            }

            const int index = call_delta_json.value("index", 0);
            auto& tool_call = tool_calls[index];
            llm_tool_call_delta call_delta {
              .index = index,
            };

            if (call_delta_json.contains("id") && call_delta_json["id"].is_string()) {
              tool_call.id = call_delta_json["id"].get<std::string>();
              call_delta.id = tool_call.id;
            }
            else {
              call_delta.id = tool_call.id;
            }

            if (call_delta_json.contains("function") &&
                call_delta_json["function"].is_object()) {
              const auto& function = call_delta_json["function"];
              if (function.contains("name") && function["name"].is_string()) {
                call_delta.name_delta = function["name"].get<std::string>();
                tool_call.name += call_delta.name_delta;
              }
              if (function.contains("arguments") && function["arguments"].is_string()) {
                call_delta.arguments_delta = function["arguments"].get<std::string>();
                tool_call.arguments_json += call_delta.arguments_delta;
              }
            }

            if (!call_delta.id.empty() || !call_delta.name_delta.empty() ||
                !call_delta.arguments_delta.empty()) {
              emitted_output = true;
              emit_stream_event(callbacks, {
                .type = llm_stream_event_type::tool_call_delta,
                .tool_call_delta = call_delta,
              });
            }
          }
        }
      }

      return true;
    };

    const auto response = http_->send_stream(
      req,
      [&](std::string_view chunk) {
        if (stop_token.stop_requested()) {
          return false;
        }
        return parser.feed(chunk, process_event);
      },
      stop_token);

    if (!stream_parse_failed) {
      parser.finish(process_event);
    }

    if (stop_token.stop_requested()) {
      result.error_code = agent::make_error_code(agent::llm_error_code::cancelled);
      emit_stream_event(callbacks, {
        .type = llm_stream_event_type::error,
        .response = result,
        .error_code = result.error_code,
      });
      return result;
    }

    if (stream_parse_failed) {
      return result;
    }

    if (response.error_code) {
      const auto body = json::parse(response.body, nullptr, false);
      result.error_code =
        classify_openai_error(response.error_code, body.is_discarded() ? json::object() : body);

      if (emitted_output && saw_sse_event) {
        result.error_code.clear();
        result.metadata["ignored_stream_transport_error"] = response.error_code.message();
      }
      else {
        result.content = openai_error_message(
          result.error_code,
          body.is_discarded() ? json::object() : body);
      }

      if (result.error_code && attempt < max_retries && !emitted_output &&
          agent::llm_detail::is_retryable_error(result.error_code) &&
          agent::llm_detail::wait_for_retry(
            stop_token,
            std::chrono::milliseconds(agent::llm_detail::compute_backoff_ms(
              attempt,
              base_backoff_ms)))) {
        continue;
      }

      if (result.error_code) {
        emit_stream_event(callbacks, {
          .type = llm_stream_event_type::error,
          .response = result,
          .error_code = result.error_code,
          .message = result.content,
        });
        return result;
      }
    }

    if (!saw_sse_event) {
      result = parse_openai_response(response);
      if (result.error_code) {
        emit_stream_event(callbacks, {
          .type = llm_stream_event_type::error,
          .response = result,
          .error_code = result.error_code,
          .message = result.content,
        });
        return result;
      }
      if (!result.content.empty()) {
        emit_stream_event(callbacks, {
          .type = llm_stream_event_type::content_delta,
          .content_delta = result.content,
        });
      }
      for (const auto& call : result.tool_calls) {
        emit_stream_event(callbacks, {
          .type = llm_stream_event_type::tool_call_done,
          .tool_call = call,
        });
      }
      emit_stream_event(callbacks, {
        .type = llm_stream_event_type::done,
        .response = result,
      });
      return result;
    }

    for (auto& [index, call] : tool_calls) {
      (void)index;
      result.tool_calls.push_back(call);
      emit_stream_event(callbacks, {
        .type = llm_stream_event_type::tool_call_done,
        .tool_call = call,
      });
    }

    if (!saw_done && result.content.empty() && result.tool_calls.empty()) {
      result.error_code = agent::make_error_code(agent::llm_error_code::invalid_response);
      result.content = "OpenAI-compatible streaming response ended without content, tool calls, or [DONE].";
      emit_stream_event(callbacks, {
        .type = llm_stream_event_type::error,
        .response = result,
        .error_code = result.error_code,
        .message = result.content,
      });
      return result;
    }

    emit_stream_event(callbacks, {
      .type = llm_stream_event_type::done,
      .response = result,
    });
    return result;
  }

  return {
    .error_code = std::make_error_code(std::errc::io_error),
  };
}

json openai_compatible_llm_client::build_openai_payload(const llm_request& request) const {
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

std::vector<std::pair<std::string, std::string>> openai_compatible_llm_client::build_headers() const {
  std::vector<std::pair<std::string, std::string>> headers {
    {"Content-Type", "application/json"},
  };
  if (!config_.api_key.empty()) {
    headers.push_back({"Authorization", "Bearer " + config_.api_key});
  }
  if (config_.base_url.find("openrouter.ai") != std::string::npos) {
    if (config_.referer_url.has_value() && !config_.referer_url->empty()) {
      headers.push_back({"HTTP-Referer", *config_.referer_url});
    }
    if (config_.app_title.has_value() && !config_.app_title->empty()) {
      headers.push_back({"X-Title", *config_.app_title});
    }
  }
  return headers;
}

llm_response openai_compatible_llm_client::parse_openai_response(const http_response& response) const {
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
