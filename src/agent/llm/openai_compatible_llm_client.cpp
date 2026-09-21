#include <wuwe/agent/llm/openai_compatible_llm_client.h>

#include "llm_endpoint.hpp"

#include "llm_retry.hpp"
#include "llm_stream_timeouts.hpp"

#include <wuwe/agent/llm/llm_error.h>
#include <wuwe/net/default_http_client.h>
#include <wuwe/net/net_errc.h>
#include <wuwe/net/sse_event_parser.h>

#include <chrono>
#include <atomic>
#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <map>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>

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
      return { { "type", "function" }, { "function", { { "name", tool_choice.name } } } };
    default:
      return "auto";
  }
}

void emit_stream_event(const llm_stream_callbacks& callbacks, llm_stream_event event) {
  if (callbacks.on_event) {
    callbacks.on_event(event);
  }
  if (event.type == llm_stream_event_type::reasoning_delta && callbacks.on_reasoning_delta &&
      !event.reasoning_delta.empty()) {
    callbacks.on_reasoning_delta(event.reasoning_delta);
  }
  if (event.type == llm_stream_event_type::reasoning_done && callbacks.on_reasoning_done &&
      !event.reasoning_summary.empty()) {
    callbacks.on_reasoning_done(event.reasoning_summary);
  }
}

std::string json_scalar_to_string(const json& value) {
  if (value.is_string()) {
    return value.get<std::string>();
  }
  if (value.is_number_integer()) {
    return std::to_string(value.get<long long>());
  }
  if (value.is_number_unsigned()) {
    return std::to_string(value.get<unsigned long long>());
  }
  if (value.is_number_float()) {
    return std::to_string(value.get<double>());
  }
  if (value.is_boolean()) {
    return value.get<bool>() ? "true" : "false";
  }
  return {};
}

std::string object_scalar_value(const json& object, const char* key) {
  if (!object.is_object()) {
    return {};
  }
  const auto it = object.find(key);
  if (it == object.end()) {
    return {};
  }
  return json_scalar_to_string(*it);
}

std::string first_string_field(const json& object, std::initializer_list<const char*> keys) {
  if (!object.is_object()) {
    return {};
  }
  for (const auto* key : keys) {
    const auto it = object.find(key);
    if (it != object.end() && it->is_string()) {
      return it->get<std::string>();
    }
  }
  return {};
}

std::string reasoning_text_from_object(const json& object) {
  const auto direct = first_string_field(object,
    {
      "reasoning_delta",
      "reasoning_summary_delta",
      "reasoning_content",
      "reasoning_summary",
      "reasoning",
      "thinking_delta",
      "thinking",
    });
  if (!direct.empty()) {
    return direct;
  }

  for (const auto* key : { "reasoning", "thinking" }) {
    const auto it = object.find(key);
    if (it != object.end() && it->is_object()) {
      const auto nested = first_string_field(*it, { "summary", "text", "content" });
      if (!nested.empty()) {
        return nested;
      }
    }
  }

  return {};
}

std::map<std::string, std::string> reasoning_metadata_from_object(const json& object) {
  std::map<std::string, std::string> metadata;
  if (!object.is_object()) {
    return metadata;
  }

  const auto signature = first_string_field(object,
    {
      "reasoning_signature",
      "thinking_signature",
      "signature",
    });
  if (!signature.empty()) {
    metadata["signature"] = signature;
  }

  for (const auto* key : {
         "reasoning_details",
         "reasoning_metadata",
         "thinking_details",
         "thinking_metadata",
       }) {
    const auto it = object.find(key);
    if (it != object.end() && (it->is_object() || it->is_array())) {
      metadata[key] = it->dump();
    }
  }

  return metadata;
}

void merge_reasoning_metadata(std::map<std::string, std::string>& destination,
  const std::map<std::string, std::string>& source) {
  for (const auto& [key, value] : source) {
    destination[key] = value;
  }
}

std::error_code classify_openai_error(
  const std::error_code& transport_or_http_error, const json& body) {
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
    const auto code = object_scalar_value(error, "code");
    const auto type = object_scalar_value(error, "type");

    if (code == "invalid_api_key" || type == "invalid_api_key" || code == "unauthorized" ||
        type == "authentication_error" || code == "401" || code == "403") {
      return agent::make_error_code(agent::llm_error_code::authentication_failed);
    }
    if (code == "rate_limit_exceeded" || type == "rate_limit_exceeded" || code == "rate_limited" ||
        code == "429") {
      return agent::make_error_code(agent::llm_error_code::rate_limited);
    }
    if (code == "model_not_found" || type == "model_not_found" || code == "model_unavailable" ||
        code == "404") {
      return agent::make_error_code(agent::llm_error_code::model_unavailable);
    }

    return agent::make_error_code(agent::llm_error_code::api_error);
  }

  return transport_or_http_error;
}

std::string openai_error_message(const std::error_code& error_code, const json& body) {
  if (body.is_object() && body.contains("error") && body["error"].is_object()) {
    const auto& error = body["error"];
    const auto explicit_message = object_scalar_value(error, "message");
    if (!explicit_message.empty()) {
      return explicit_message;
    }
    const auto type = object_scalar_value(error, "type");
    const auto code = object_scalar_value(error, "code");
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
    return "OpenAI-compatible request failed: " + error_code.message();
  }
  return "OpenAI-compatible request failed.";
}

std::string invalid_stream_event_message() {
  return "Invalid OpenAI-compatible streaming event.";
}

std::string build_chat_completions_url(const llm_client_config& config) {
  return agent::llm::detail::openai_chat_endpoint(config.base_url, config.chat_completions_path);
}

std::string trim_copy(std::string value) {
  const auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
  value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), is_space));
  value.erase(std::find_if_not(value.rbegin(), value.rend(), is_space).base(), value.end());
  return value;
}

void replace_all(std::string& value, std::string_view from, std::string_view to) {
  std::size_t offset = 0;
  while ((offset = value.find(from, offset)) != std::string::npos) {
    value.replace(offset, from.size(), to);
    offset += to.size();
  }
}

std::string canonicalize_dsml(std::string value) {
  replace_all(value, "<｜DSML｜", "<|DSML|");
  replace_all(value, "</｜DSML｜", "</|DSML|");
  replace_all(value, "“", "\"");
  replace_all(value, "”", "\"");
  return value;
}

class dsml_stream_filter {
public:
  std::string feed(std::string_view chunk) {
    pending_.append(chunk.data(), chunk.size());
    if (protocol_detected_) {
      return {};
    }

    const auto ascii = pending_.find(kAsciiMarker);
    const auto fullwidth = pending_.find(kFullwidthMarker);
    auto marker = std::min(ascii, fullwidth);
    if (ascii == std::string::npos) {
      marker = fullwidth;
    }
    else if (fullwidth == std::string::npos) {
      marker = ascii;
    }
    if (marker != std::string::npos) {
      protocol_detected_ = true;
      auto visible = pending_.substr(0, marker);
      pending_.erase(0, marker);
      return visible;
    }

    const auto retained = marker_prefix_suffix_size(pending_);
    auto visible = pending_.substr(0, pending_.size() - retained);
    pending_.erase(0, pending_.size() - retained);
    return visible;
  }

  std::string finish(bool suppress_protocol) {
    if (protocol_detected_ && suppress_protocol) {
      pending_.clear();
      return {};
    }
    return std::exchange(pending_, {});
  }

private:
  static std::size_t marker_prefix_suffix_size(const std::string& value) {
    std::size_t retained = 0;
    for (const auto marker : { kAsciiMarker, kFullwidthMarker }) {
      const auto limit = (std::min)(value.size(), marker.size() - 1);
      for (std::size_t size = 1; size <= limit; ++size) {
        if (value.compare(value.size() - size, size, marker.data(), size) == 0) {
          retained = (std::max)(retained, size);
        }
      }
    }
    return retained;
  }

  static constexpr std::string_view kAsciiMarker = "<|DSML|tool_calls";
  static constexpr std::string_view kFullwidthMarker = "<｜DSML｜tool_calls";
  std::string pending_;
  bool protocol_detected_ { false };
};

std::optional<std::string> attribute_value(
  const std::string& attributes, std::string_view name) {
  const std::regex expression(
    std::string(R"re((?:^|\s))re") + std::string(name) +
      R"re(\s*=\s*["']([^"']*)["'])re",
    std::regex::icase);
  std::smatch match;
  if (!std::regex_search(attributes, match, expression)) {
    return std::nullopt;
  }
  return match[1].str();
}

std::optional<std::vector<llm_tool_call>> parse_dsml_tool_calls(
  const std::string& content, const std::vector<llm_tool>& available_tools,
  std::size_t& protocol_offset) {
  static std::atomic<std::uint64_t> next_call_id { 1 };
  constexpr std::size_t max_protocol_bytes = 1024 * 1024;
  if (content.size() > max_protocol_bytes) {
    return std::nullopt;
  }
  const auto canonical = canonicalize_dsml(content);
  const auto envelope = canonical.find("<|DSML|tool_calls");
  if (envelope == std::string::npos) {
    return std::nullopt;
  }
  const auto envelope_start_end = canonical.find('>', envelope);
  constexpr std::string_view envelope_closing = "</|DSML|tool_calls>";
  const auto envelope_end = canonical.find(envelope_closing, envelope);
  if (envelope_start_end == std::string::npos || envelope_end == std::string::npos ||
      envelope_start_end >= envelope_end ||
      !trim_copy(canonical.substr(envelope_end + envelope_closing.size())).empty()) {
    return std::nullopt;
  }
  protocol_offset = envelope;
  const auto protocol = canonical.substr(
    envelope_start_end + 1, envelope_end - envelope_start_end - 1);

  const std::unordered_set<std::string> registered_tools = [&] {
    std::unordered_set<std::string> names;
    for (const auto& tool : available_tools) {
      names.insert(tool.name);
    }
    return names;
  }();
  const std::regex invoke_expression(
    R"re(<\|DSML\|invoke\s+([^>]*)>([\s\S]*?)</\|DSML\|invoke\s*>)re",
    std::regex::icase);
  const std::regex parameter_expression(
    R"re(<\|DSML\|(?:invokeParameter|parameter)\s+([^>]*)>([\s\S]*?)</\|DSML\|(?:invokeParameter|parameter)\s*>)re",
    std::regex::icase);

  std::vector<llm_tool_call> calls;
  for (auto invoke = std::sregex_iterator(protocol.begin(), protocol.end(), invoke_expression);
       invoke != std::sregex_iterator(); ++invoke) {
    const auto name = attribute_value((*invoke)[1].str(), "name");
    if (!name || !registered_tools.contains(*name)) {
      return std::nullopt;
    }

    json arguments = json::object();
    const auto body = (*invoke)[2].str();
    for (auto parameter = std::sregex_iterator(body.begin(), body.end(), parameter_expression);
         parameter != std::sregex_iterator(); ++parameter) {
      const auto attributes = (*parameter)[1].str();
      const auto parameter_name = attribute_value(attributes, "name");
      if (!parameter_name || parameter_name->empty() || arguments.contains(*parameter_name)) {
        return std::nullopt;
      }
      const auto string_attribute = attribute_value(attributes, "string");
      const auto raw_value = trim_copy((*parameter)[2].str());
      if (!string_attribute || *string_attribute == "true" || *string_attribute == "1") {
        arguments[*parameter_name] = raw_value;
      }
      else if (*string_attribute == "false" || *string_attribute == "0") {
        auto parsed = json::parse(raw_value, nullptr, false);
        if (parsed.is_discarded()) {
          return std::nullopt;
        }
        arguments[*parameter_name] = std::move(parsed);
      }
      else {
        return std::nullopt;
      }
    }
    calls.push_back({
      .id = "wuwe-dsml-" +
            std::to_string(next_call_id.fetch_add(1, std::memory_order_relaxed)),
      .name = *name,
      .arguments_json = arguments.dump(),
    });
  }
  if (calls.empty()) {
    return std::nullopt;
  }
  return calls;
}

bool requires_explicit_tool_choice(const llm_request& request) {
  return request.tool_choice &&
         (request.tool_choice->mode == llm_tool_choice_mode::required ||
           request.tool_choice->mode == llm_tool_choice_mode::named);
}

bool is_explicit_tool_choice_unsupported(const llm_response& response) {
  if (!response.error_code) {
    return false;
  }
  auto message = response.content;
  std::transform(message.begin(), message.end(), message.begin(),
    [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return message.find("tool_choice") != std::string::npos &&
         (message.find("not support") != std::string::npos ||
           message.find("unsupported") != std::string::npos ||
           message.find("does not support") != std::string::npos ||
           message.find("不支持") != std::string::npos);
}

} // namespace

openai_compatible_llm_client::openai_compatible_llm_client(llm_client_config config)
    : config_(normalize_config(std::move(config))), http_(std::make_shared<default_http_client>()) {
}

openai_compatible_llm_client::openai_compatible_llm_client(
  llm_client_config config, std::shared_ptr<http_client> http)
    : openai_compatible_llm_client(
        std::move(config), std::move(http), openai_compatibility_policy {}) {
}

openai_compatible_llm_client::openai_compatible_llm_client(llm_client_config config,
  std::shared_ptr<http_client> http, openai_compatibility_policy policy)
    : config_(normalize_config(std::move(config))), http_(std::move(http)),
      compatibility_policy_(policy) {
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

llm_response openai_compatible_llm_client::complete(
  const llm_request& request, std::stop_token stop_token) {
  if (auto rejected = agent::llm::llm_request_rejection(request, capabilities())) {
    return std::move(*rejected);
  }
  if (stop_token.stop_requested()) {
    return { .error_code = agent::make_error_code(agent::llm_error_code::cancelled) };
  }
  if (config_.require_api_key && config_.api_key.empty()) {
    return { .error_code = agent::make_error_code(agent::llm_error_code::missing_api_key) };
  }

  llm_request effective_request = request;
  if (compatibility_policy_.negotiate_explicit_tool_choice &&
      explicit_tool_choice_unsupported_.load(std::memory_order_acquire) &&
      requires_explicit_tool_choice(effective_request)) {
    effective_request.tool_choice.reset();
  }
  const auto payload = build_openai_payload(effective_request);

  const http_request req {
    .method = "POST",
    .url = build_chat_completions_url(config_),
    .headers = build_headers(),
    .body = payload.dump(),
    .timeout = config_.timeout,
    .trace_id = request.execution_context ? request.execution_context->trace_id : std::string {},
  };

  const int max_retries = config_.max_retries < 0 ? 0 : config_.max_retries;
  for (int attempt = 0; attempt <= max_retries; ++attempt) {
    if (stop_token.stop_requested()) {
      return { .error_code = agent::make_error_code(agent::llm_error_code::cancelled) };
    }

    const auto response = http_->send(req);
    llm_response parsed = normalize_provider_response(
      effective_request, parse_openai_response(response));
    agent::llm_detail::apply_retry_metadata(parsed, response);
    apply_reasoning_language_metadata(parsed,
      effective_request.language,
      has_language_preferences(effective_request.language)
        ? llm_reasoning_language_control::prompt_contract
        : llm_reasoning_language_control::unsupported);
    if (stop_token.stop_requested()) {
      return { .error_code = agent::make_error_code(agent::llm_error_code::cancelled) };
    }
    if (!parsed.error_code) {
      return parsed;
    }
    if (compatibility_policy_.negotiate_explicit_tool_choice &&
        requires_explicit_tool_choice(effective_request) &&
        is_explicit_tool_choice_unsupported(parsed)) {
      explicit_tool_choice_unsupported_.store(true, std::memory_order_release);
      auto compatible_request = request;
      compatible_request.tool_choice.reset();
      auto retried = complete(compatible_request, stop_token);
      retried.metadata["wuwe_tool_choice_fallback"] = "provider_default";
      return retried;
    }
    if (attempt >= max_retries || !agent::llm_detail::is_retryable_error(parsed.error_code)) {
      return parsed;
    }
    if (!agent::llm_detail::wait_for_retry(
          stop_token, agent::llm_detail::compute_retry_delay(config_, attempt, response))) {
      return { .error_code = agent::make_error_code(agent::llm_error_code::cancelled) };
    }
  }

  llm_response fallback;
  fallback.error_code = std::make_error_code(std::errc::io_error);
  return fallback;
}

llm_response openai_compatible_llm_client::complete_stream(
  const llm_request& request, const llm_stream_callbacks& callbacks, std::stop_token stop_token) {
  if (auto rejected = agent::llm::llm_request_rejection(request, capabilities())) {
    agent::llm::emit_llm_request_rejection(callbacks, *rejected);
    return std::move(*rejected);
  }
  if (stop_token.stop_requested()) {
    auto response = llm_response {
      .error_code = agent::make_error_code(agent::llm_error_code::cancelled),
    };
    emit_stream_event(callbacks,
      {
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
    emit_stream_event(callbacks,
      {
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
    .trace_id = request.execution_context ? request.execution_context->trace_id : std::string {},
  };

  const int max_retries = config_.max_retries < 0 ? 0 : config_.max_retries;

  for (int attempt = 0; attempt <= max_retries; ++attempt) {
    if (stop_token.stop_requested()) {
      auto response = llm_response {
        .error_code = agent::make_error_code(agent::llm_error_code::cancelled),
      };
      emit_stream_event(callbacks,
        {
          .type = llm_stream_event_type::error,
          .response = response,
          .error_code = response.error_code,
        });
      return response;
    }

    llm_response result;
    const auto reasoning_language_control = has_language_preferences(request.language)
                                              ? llm_reasoning_language_control::prompt_contract
                                              : llm_reasoning_language_control::unsupported;
    sse_event_parser parser;
    std::map<int, llm_tool_call> tool_calls;
    bool emitted_output = false;
    bool saw_sse_event = false;
    bool saw_done = false;
    bool stream_parse_failed = false;
    std::optional<dsml_stream_filter> text_tool_filter;
    if (compatibility_policy_.buffer_text_tool_protocol && !request.tools.empty()) {
      text_tool_filter.emplace();
    }
    agent::llm_detail::stream_timeout_guard timeout_guard(config_.stream_timeouts);

    const auto fail_stream = [&](std::error_code error_code, std::string content) {
      result.error_code = error_code;
      result.content = std::move(content);
      stream_parse_failed = true;
      emit_stream_event(callbacks,
        {
          .type = llm_stream_event_type::error,
          .response = result,
          .error_code = result.error_code,
          .message = result.content,
        });
      return false;
    };
    const auto fail_stream_timeout = [&](std::string phase, int timeout_ms) {
      result.metadata["timeout_phase"] = phase;
      result.metadata["timeout_ms"] = std::to_string(timeout_ms);
      return fail_stream(agent::make_error_code(agent::llm_error_code::timeout),
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
        return fail_stream(agent::make_error_code(agent::llm_error_code::invalid_response),
          invalid_stream_event_message());
      }

      if (data.contains("error") && data["error"].is_object()) {
        return fail_stream(
          classify_openai_error(agent::make_error_code(agent::llm_error_code::api_error), data),
          openai_error_message(agent::make_error_code(agent::llm_error_code::api_error), data));
      }

      if (data.contains("usage") && data["usage"].is_object()) {
        const auto& usage = data["usage"];
        result.usage.prompt_tokens = usage.value("prompt_tokens", 0);
        result.usage.completion_tokens = usage.value("completion_tokens", 0);
        result.usage.total_tokens = usage.value("total_tokens", 0);
        if (usage.contains("prompt_tokens_details") && usage["prompt_tokens_details"].is_object()) {
          result.usage.cached_prompt_tokens =
            usage["prompt_tokens_details"].value("cached_tokens", 0);
        }
        if (usage.contains("completion_tokens_details") &&
            usage["completion_tokens_details"].is_object()) {
          result.usage.reasoning_tokens =
            usage["completion_tokens_details"].value("reasoning_tokens", 0);
        }
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
        const auto reasoning_delta = reasoning_text_from_object(delta);
        if (!reasoning_delta.empty()) {
          result.reasoning_summary += reasoning_delta;
          merge_reasoning_metadata(
            result.reasoning_metadata, reasoning_metadata_from_object(delta));
          merge_reasoning_language_metadata(result.reasoning_metadata,
            request.language,
            reasoning_language_control,
            reasoning_delta);
          emitted_output = true;
          emit_stream_event(callbacks,
            {
              .type = llm_stream_event_type::reasoning_delta,
              .reasoning_delta = reasoning_delta,
              .reasoning_metadata = result.reasoning_metadata,
            });
        }

        if (delta.contains("content") && delta["content"].is_string()) {
          const auto content_delta = delta["content"].get<std::string>();
          if (!content_delta.empty()) {
            result.content += content_delta;
            emitted_output = true;
            const auto visible_delta = text_tool_filter
              ? text_tool_filter->feed(content_delta)
              : content_delta;
            if (!visible_delta.empty()) {
              emit_stream_event(callbacks,
                {
                  .type = llm_stream_event_type::content_delta,
                  .content_delta = visible_delta,
                });
            }
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

            if (call_delta_json.contains("function") && call_delta_json["function"].is_object()) {
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
              emit_stream_event(callbacks,
                {
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
    agent::llm_detail::apply_retry_metadata(result, response);

    if (!stream_parse_failed) {
      parser.finish(process_event);
    }

    if (stop_token.stop_requested()) {
      result.error_code = agent::make_error_code(agent::llm_error_code::cancelled);
      emit_stream_event(callbacks,
        {
          .type = llm_stream_event_type::error,
          .response = result,
          .error_code = result.error_code,
        });
      return result;
    }

    if (stream_parse_failed) {
      if (!emitted_output && compatibility_policy_.negotiate_explicit_tool_choice &&
          requires_explicit_tool_choice(request) &&
          is_explicit_tool_choice_unsupported(result)) {
        explicit_tool_choice_unsupported_.store(true, std::memory_order_release);
        auto compatible_request = request;
        compatible_request.tool_choice.reset();
        auto retried = complete_stream(compatible_request, callbacks, stop_token);
        retried.metadata["wuwe_tool_choice_fallback"] = "provider_default";
        return retried;
      }
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
        result.content =
          openai_error_message(result.error_code, body.is_discarded() ? json::object() : body);
      }

      if (result.error_code && !emitted_output &&
          compatibility_policy_.negotiate_explicit_tool_choice &&
          requires_explicit_tool_choice(request) &&
          is_explicit_tool_choice_unsupported(result)) {
        explicit_tool_choice_unsupported_.store(true, std::memory_order_release);
        auto compatible_request = request;
        compatible_request.tool_choice.reset();
        auto retried = complete_stream(compatible_request, callbacks, stop_token);
        retried.metadata["wuwe_tool_choice_fallback"] = "provider_default";
        return retried;
      }

      if (result.error_code && attempt < max_retries && !emitted_output &&
          agent::llm_detail::is_retryable_error(result.error_code) &&
          agent::llm_detail::wait_for_retry(
            stop_token, agent::llm_detail::compute_retry_delay(config_, attempt, response))) {
        continue;
      }

      if (result.error_code) {
        emit_stream_event(callbacks,
          {
            .type = llm_stream_event_type::error,
            .response = result,
            .error_code = result.error_code,
            .message = result.content,
          });
        return result;
      }
    }

    if (!saw_sse_event) {
      result = normalize_provider_response(request, parse_openai_response(response));
      apply_reasoning_language_metadata(result, request.language, reasoning_language_control);
      if (result.error_code) {
        emit_stream_event(callbacks,
          {
            .type = llm_stream_event_type::error,
            .response = result,
            .error_code = result.error_code,
            .message = result.content,
          });
        return result;
      }
      if (!result.content.empty()) {
        emit_stream_event(callbacks,
          {
            .type = llm_stream_event_type::content_delta,
            .content_delta = result.content,
          });
      }
      if (!result.reasoning_summary.empty()) {
        emit_stream_event(callbacks,
          {
            .type = llm_stream_event_type::reasoning_done,
            .reasoning_summary = result.reasoning_summary,
            .reasoning_metadata = result.reasoning_metadata,
            .response = result,
          });
      }
      for (const auto& call : result.tool_calls) {
        emit_stream_event(callbacks,
          {
            .type = llm_stream_event_type::tool_call_done,
            .tool_call = call,
          });
      }
      if (result.content.empty() && result.tool_calls.empty()) {
        result.error_code = agent::make_error_code(agent::llm_error_code::invalid_response);
        result.stop_reason = "empty_terminal_response";
        result.metadata["finish_reason"] = result.finish_reason;
        result.metadata["reasoning_bytes"] = std::to_string(result.reasoning_summary.size());
        result.content =
          "OpenAI-compatible response ended without final content or a tool call.";
        emit_stream_event(callbacks,
          {
            .type = llm_stream_event_type::error,
            .response = result,
            .error_code = result.error_code,
            .message = result.content,
          });
        return result;
      }
      emit_stream_event(callbacks,
        {
          .type = llm_stream_event_type::done,
          .response = result,
        });
      return result;
    }

    for (auto& [index, call] : tool_calls) {
      (void)index;
      result.tool_calls.push_back(call);
    }
    result = normalize_provider_response(request, std::move(result));
    if (text_tool_filter) {
      const bool normalized_protocol =
        result.metadata.contains("wuwe_tool_protocol_normalized");
      const auto tail = text_tool_filter->finish(normalized_protocol);
      if (!tail.empty()) {
        emit_stream_event(callbacks,
          {
            .type = llm_stream_event_type::content_delta,
            .content_delta = tail,
          });
      }
    }
    for (const auto& call : result.tool_calls) {
      emit_stream_event(callbacks,
        {
          .type = llm_stream_event_type::tool_call_done,
          .tool_call = call,
        });
    }

    if (result.content.empty() && result.tool_calls.empty()) {
      result.error_code = agent::make_error_code(agent::llm_error_code::invalid_response);
      result.stop_reason = "empty_terminal_response";
      result.metadata["finish_reason"] = result.finish_reason;
      result.metadata["saw_done"] = saw_done ? "true" : "false";
      result.metadata["reasoning_bytes"] = std::to_string(result.reasoning_summary.size());
      result.content = "OpenAI-compatible streaming response ended without final content or a tool call.";
      emit_stream_event(callbacks,
        {
          .type = llm_stream_event_type::error,
          .response = result,
          .error_code = result.error_code,
          .message = result.content,
        });
      return result;
    }

    if (!result.reasoning_summary.empty()) {
      apply_reasoning_language_metadata(result, request.language, reasoning_language_control);
      emit_stream_event(callbacks,
        {
          .type = llm_stream_event_type::reasoning_done,
          .reasoning_summary = result.reasoning_summary,
          .reasoning_metadata = result.reasoning_metadata,
          .response = result,
        });
    }
    emit_stream_event(callbacks,
      {
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
  const auto language_contract = llm_language_contract(request.language);
  if (!language_contract.empty()) {
    messages.push_back({ { "role", "system" }, { "content", language_contract } });
  }

  for (const auto& msg : request.messages) {
    json message = { { "role", msg.role }, { "content", msg.content } };
    if (compatibility_policy_.replay_reasoning_content && !msg.reasoning_content.empty()) {
      message["reasoning_content"] = msg.reasoning_content;
    }
    if (msg.name.has_value()) {
      message["name"] = *msg.name;
    }
    if (msg.tool_call_id.has_value()) {
      message["tool_call_id"] = *msg.tool_call_id;
    }
    if (!msg.tool_calls.empty()) {
      auto tool_calls = json::array();
      for (const auto& call : msg.tool_calls) {
        tool_calls.push_back({ { "id", call.id },
          { "type", "function" },
          { "function", { { "name", call.name }, { "arguments", call.arguments_json } } } });
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

  if (request.max_output_tokens && *request.max_output_tokens > 0) {
    const auto* limit_key =
      compatibility_policy_.use_max_completion_tokens ? "max_completion_tokens" : "max_tokens";
    payload[limit_key] = *request.max_output_tokens;
  }

  if (compatibility_policy_.request_thinking_control &&
      request.thinking_mode != llm_thinking_mode::provider_default) {
    payload["thinking"] = {
      { "type", request.thinking_mode == llm_thinking_mode::enabled ? "enabled" : "disabled" },
    };
  }

  if (!request.stop_sequences.empty()) {
    payload["stop"] = request.stop_sequences;
  }
  if (request.seed) {
    payload["seed"] = *request.seed;
  }

  if (request.response_format.has_value()) {
    if (*request.response_format == "json_object") {
      payload["response_format"] = { { "type", "json_object" } };
    }
  }
  else if (request.json_schema_output) {
    payload["response_format"] = {
      { "type", "json_schema" },
      { "json_schema",
        {
          { "name", request.json_schema_output->name },
          { "strict", request.json_schema_output->strict },
          { "schema", request.json_schema_output->schema },
        } },
    };
  }

  if (!request.tools.empty()) {
    auto tools = json::array();
    for (const auto& tool : request.tools) {
      tools.push_back({ { "type", "function" },
        { "function",
          { { "name", tool.name },
            { "description", tool.description },
            { "parameters",
              parse_json_object_or_default(tool.parameters_json_schema, json::object()) } } } });
    }
    payload["tools"] = std::move(tools);
  }

  if (request.tool_choice.has_value()) {
    payload["tool_choice"] = build_tool_choice_json(*request.tool_choice);
  }

  return payload;
}

std::vector<std::pair<std::string, std::string>>
openai_compatible_llm_client::build_headers() const {
  std::vector<std::pair<std::string, std::string>> headers {
    { "Content-Type", "application/json" },
  };
  if (!config_.api_key.empty()) {
    headers.push_back({ "Authorization", "Bearer " + config_.api_key });
  }
  if (config_.base_url.find("openrouter.ai") != std::string::npos) {
    if (config_.referer_url.has_value() && !config_.referer_url->empty()) {
      headers.push_back({ "HTTP-Referer", *config_.referer_url });
    }
    if (config_.app_title.has_value() && !config_.app_title->empty()) {
      headers.push_back({ "X-Title", *config_.app_title });
    }
  }
  return headers;
}

llm_response openai_compatible_llm_client::parse_openai_response(
  const http_response& response) const {
  llm_response result;

  if (response.error_code) {
    const auto body = json::parse(response.body, nullptr, false);
    result.error_code =
      classify_openai_error(response.error_code, body.is_discarded() ? json::object() : body);
    result.content =
      openai_error_message(result.error_code, body.is_discarded() ? json::object() : body);
    return result;
  }

  const auto data = json::parse(response.body, nullptr, false);
  if (data.is_discarded()) {
    result.error_code = agent::make_error_code(agent::llm_error_code::invalid_response);
    result.content = response.body;
    return result;
  }

  if (data.contains("error") && data["error"].is_object()) {
    result.error_code =
      classify_openai_error(agent::make_error_code(agent::llm_error_code::api_error), data);
    result.content = openai_error_message(result.error_code, data);
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

  result.reasoning_summary = reasoning_text_from_object(message);
  result.reasoning_metadata = reasoning_metadata_from_object(message);

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
    if (usage.contains("prompt_tokens_details") && usage["prompt_tokens_details"].is_object()) {
      result.usage.cached_prompt_tokens = usage["prompt_tokens_details"].value("cached_tokens", 0);
    }
    if (usage.contains("completion_tokens_details") &&
        usage["completion_tokens_details"].is_object()) {
      result.usage.reasoning_tokens =
        usage["completion_tokens_details"].value("reasoning_tokens", 0);
    }
  }

  return result;
}

llm_response openai_compatible_llm_client::normalize_provider_response(
  const llm_request& request, llm_response response) const {
  if (!compatibility_policy_.normalize_dsml_tool_calls || response.error_code ||
      !response.tool_calls.empty() || request.tools.empty() || response.content.empty()) {
    return response;
  }

  std::size_t protocol_offset = 0;
  const auto calls = parse_dsml_tool_calls(response.content, request.tools, protocol_offset);
  if (!calls) {
    return response;
  }
  response.content = trim_copy(response.content.substr(0, protocol_offset));
  response.tool_calls = *calls;
  response.metadata["wuwe_tool_protocol_normalized"] = "dsml";
  return response;
}

WUWE_NAMESPACE_END
