#include <wuwe/agent/llm/llm_model_discovery.h>

#include "llm_endpoint.hpp"

#include <algorithm>
#include <chrono>
#include <unordered_set>
#include <utility>

#include <nlohmann/json.hpp>

#include <wuwe/agent/llm/llm_provider_registry.h>
#include <wuwe/net/default_http_client.h>
#include <wuwe/net/http_status_code.h>
#include <wuwe/net/transport_error.h>

WUWE_NAMESPACE_BEGIN

namespace {

using agent::llm_error_code;
using json = nlohmann::json;
using clock_type = std::chrono::steady_clock;

llm_model_list_result failure(llm_error_code code, int status = 0, std::error_code transport = {}) {
  return {
    .error_code = agent::make_error_code(code), .transport_error = transport, .http_status = status
  };
}

bool has_control(std::string_view value) {
  return std::any_of(
    value.begin(), value.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; });
}

bool valid_path(std::string_view path) {
  return !path.empty() && path.front() == '/' && !path.starts_with("//") &&
         path.find_first_of(" ?#\\") == std::string_view::npos && !has_control(path);
}

bool valid_base_url(std::string_view url) {
  const auto offset = url.starts_with("https://") ? 8u : url.starts_with("http://") ? 7u : 0u;
  if (offset == 0 || has_control(url) || url.find_first_of(" ?#\\") != std::string_view::npos) {
    return false;
  }
  const auto end = url.find('/', offset);
  const auto authority = url.substr(offset, end == std::string_view::npos ? end : end - offset);
  return !authority.empty() && authority.find('@') == std::string_view::npos;
}

std::optional<llm_model_list_format> provider_format(llm_provider_protocol protocol) {
  switch (protocol) {
    case llm_provider_protocol::openai_compatible:
      return llm_model_list_format::openai;
    case llm_provider_protocol::anthropic_messages:
      return llm_model_list_format::anthropic;
    case llm_provider_protocol::gemini_generate_content:
      return llm_model_list_format::gemini;
    case llm_provider_protocol::ollama_chat:
      return llm_model_list_format::ollama;
  }
  return std::nullopt;
}

std::string model_list_url(const llm_client_config& config, llm_model_list_format format,
  const llm_model_list_options& options) {
  auto base = config.base_url;
  while (!base.empty() && base.back() == '/') {
    base.pop_back();
  }
  if (!options.models_path.empty()) {
    return base + options.models_path;
  }
  const auto base_path = agent::llm::detail::endpoint_base_path(base);
  switch (format) {
    case llm_model_list_format::openai: {
      constexpr std::string_view suffix = "/chat/completions";
      const std::string_view chat_path = config.chat_completions_path.empty()
                                           ? std::string_view { "/v1/chat/completions" }
                                           : std::string_view { config.chat_completions_path };
      if (!chat_path.ends_with(suffix)) {
        return {}; // A nonstandard generation route needs an explicit models path.
      }
      if (!valid_path(chat_path.front() == '/' ? std::string { chat_path }
                                               : "/" + std::string { chat_path })) {
        return {};
      }
      auto endpoint = agent::llm::detail::openai_chat_endpoint(base, std::string { chat_path });
      endpoint.replace(endpoint.size() - suffix.size(), suffix.size(), "/models");
      return endpoint;
    }
    case llm_model_list_format::anthropic:
      return agent::llm::detail::api_endpoint(base, "/v1", "/models");
    case llm_model_list_format::gemini:
      return agent::llm::detail::api_endpoint(
        base, base_path.ends_with("/v1") ? "/v1" : "/v1beta", "/models");
    case llm_model_list_format::ollama:
      return agent::llm::detail::api_endpoint(base, "/api", "/tags");
  }
  return {};
}

std::string encode_query(std::string_view value) {
  constexpr char hex[] = "0123456789ABCDEF";
  std::string encoded;
  for (unsigned char c : value) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
        c == '_' || c == '.' || c == '~') {
      encoded += static_cast<char>(c);
    }
    else {
      encoded += '%';
      encoded += hex[c >> 4];
      encoded += hex[c & 15];
    }
  }
  return encoded;
}

std::vector<std::pair<std::string, std::string>> model_list_headers(
  const llm_client_config& config, llm_model_list_format format) {
  std::vector<std::pair<std::string, std::string>> headers { { "Accept", "application/json" } };
  if (format == llm_model_list_format::anthropic) {
    headers.emplace_back("anthropic-version", "2023-06-01");
  }
  if (!config.api_key.empty()) {
    if (format == llm_model_list_format::anthropic) {
      headers.emplace_back("x-api-key", config.api_key);
    }
    else if (format == llm_model_list_format::gemini) {
      headers.emplace_back("x-goog-api-key", config.api_key);
    }
    else {
      headers.emplace_back("Authorization", "Bearer " + config.api_key);
    }
  }
  return headers;
}

llm_error_code response_error(const http_response& response) {
  const auto error = response.transport_error ? response.transport_error : response.error_code;
  if (error == std::errc::operation_canceled ||
      error == make_error_code(transport_error::aborted_by_callback)) {
    return llm_error_code::cancelled;
  }
  if (error == std::errc::timed_out ||
      error == make_error_code(transport_error::operation_timedout)) {
    return llm_error_code::timeout;
  }
  // HTTP backends also populate error_code for HTTP failures.
  if (response.transport_error || (error && error.category() != http_status_category())) {
    return llm_error_code::transport_error;
  }
  switch (response.status_code) {
    case 401:
    case 403:
      return llm_error_code::authentication_failed;
    case 404:
    case 405:
    case 501:
      return llm_error_code::unsupported_capability;
    case 408:
    case 504:
      return llm_error_code::timeout;
    case 429:
      return llm_error_code::rate_limited;
    default:
      return response.status_code >= 200 && response.status_code < 300 ? llm_error_code::none
                                                                       : llm_error_code::http_error;
  }
}

struct parsed_page {
  std::vector<llm_model_info> models;
  std::string next_cursor;
  llm_error_code error { llm_error_code::none };
};

bool read_optional_string(
  const json& object, const char* key, std::string& value, bool allow_null = false) {
  const auto found = object.find(key);
  if (found == object.end() || (allow_null && found->is_null())) {
    return true;
  }
  if (!found->is_string()) {
    return false;
  }
  value = found->get<std::string>();
  return !has_control(value);
}

parsed_page parse_page(std::string_view body, llm_model_list_format format) {
  parsed_page page;
  const auto invalid = [] { return parsed_page { .error = llm_error_code::invalid_response }; };
  const auto root = json::parse(body, nullptr, false);
  if (!root.is_object()) {
    return invalid();
  }
  if (root.contains("error")) {
    return { .error = llm_error_code::api_error };
  }
  const bool data_format =
    format == llm_model_list_format::openai || format == llm_model_list_format::anthropic;
  auto items = root.find(data_format ? "data" : "models");
  // Zhipu's alternate catalog uses models[].slug. Only accept this fallback
  // for compatible discovery when data is absent; malformed data must fail.
  const bool slug_format =
    format == llm_model_list_format::openai && items == root.end() && root.contains("models");
  if (slug_format) {
    items = root.find("models");
  }
  // Protobuf JSON can omit an empty repeated field in Gemini responses.
  if (items == root.end() && (format != llm_model_list_format::gemini ||
                               (!root.empty() && !root.contains("nextPageToken")))) {
    return invalid();
  }
  if (items != root.end()) {
    if (!items->is_array()) {
      return invalid();
    }
    for (const auto& item : *items) {
      if (!item.is_object()) {
        return invalid();
      }
      llm_model_info model;
      const auto* id_field = slug_format ? "slug" : data_format ? "id" : "name";
      if (!read_optional_string(item, id_field, model.id) || model.id.empty()) {
        return invalid();
      }
      if (format == llm_model_list_format::gemini && model.id.starts_with("models/")) {
        model.id.erase(0, 7);
        if (model.id.empty()) {
          return invalid();
        }
      }
      std::string display;
      if (!read_optional_string(item,
            format == llm_model_list_format::gemini ? "displayName" : "display_name",
            display,
            true)) {
        return invalid();
      }
      // OpenRouter and some compatible catalogs expose a human-readable name.
      if (format == llm_model_list_format::openai && display.empty() &&
          !read_optional_string(item, "name", display, true)) {
        return invalid();
      }
      if (!display.empty()) {
        model.display_name = std::move(display);
      }
      page.models.push_back(std::move(model));
    }
  }
  if (format == llm_model_list_format::gemini) {
    if (!read_optional_string(root, "nextPageToken", page.next_cursor)) {
      return invalid();
    }
  }
  else if (data_format) {
    const auto more = root.find("has_more");
    if (more != root.end()) {
      if (!more->is_boolean()) {
        return invalid();
      }
      if (more->get<bool>()) {
        if (!read_optional_string(root, "last_id", page.next_cursor) || page.next_cursor.empty() ||
            page.models.empty()) {
          return invalid();
        }
      }
    }
  }
  return page;
}

} // namespace

llm_model_list_result list_llm_models(std::string_view provider_id, llm_client_config config,
  const llm_model_list_options& options, std::stop_token stop) {
  default_http_client http;
  return list_llm_models(provider_id, std::move(config), http, options, stop);
}

llm_model_list_result list_llm_models(std::string_view provider_id, llm_client_config config,
  http_client& http, const llm_model_list_options& options, std::stop_token stop) {
  if (stop.stop_requested()) {
    return failure(llm_error_code::cancelled);
  }
  const auto* provider = find_llm_provider(provider_id);
  if (!provider || config.timeout <= 0 || options.max_pages == 0 || options.max_models == 0 ||
      options.max_response_bytes == 0 ||
      (!options.models_path.empty() && !valid_path(options.models_path))) {
    return failure(llm_error_code::invalid_request);
  }
  config = normalize_llm_client_config(*provider, std::move(config));
  if (!valid_base_url(config.base_url) || has_control(config.api_key)) {
    return failure(llm_error_code::invalid_request);
  }
  if (config.require_api_key && config.api_key.empty()) {
    return failure(llm_error_code::missing_api_key);
  }
  const auto format = options.format ? options.format : provider_format(provider->protocol);
  if (!format ||
      (*format != llm_model_list_format::openai && *format != llm_model_list_format::anthropic &&
        *format != llm_model_list_format::gemini && *format != llm_model_list_format::ollama)) {
    return failure(llm_error_code::invalid_request);
  }
  const auto url = model_list_url(config, *format, options);
  if (url.empty()) {
    return failure(llm_error_code::invalid_request);
  }
  const auto headers = model_list_headers(config, *format);
  const auto deadline = clock_type::now() + std::chrono::milliseconds(config.timeout);
  llm_model_list_result result;
  std::unordered_set<std::string> ids;
  std::unordered_set<std::string> cursors;
  std::string cursor;
  for (std::size_t page_index = 0; page_index < options.max_pages; ++page_index) {
    if (stop.stop_requested()) {
      return failure(llm_error_code::cancelled, result.http_status);
    }
    const auto remaining =
      std::chrono::ceil<std::chrono::milliseconds>(deadline - clock_type::now()).count();
    if (remaining <= 0) {
      return failure(llm_error_code::timeout, result.http_status);
    }
    auto page_url = url;
    if (!cursor.empty()) {
      page_url += *format == llm_model_list_format::gemini      ? "?pageToken="
                  : *format == llm_model_list_format::anthropic ? "?after_id="
                                                                : "?after=";
      page_url += encode_query(cursor);
    }
    const http_request request {
      .method = "GET",
      .url = std::move(page_url),
      .headers = headers,
      .timeout = static_cast<int>(remaining),
      .follow_redirects = false,
    };
    std::string body;
    bool too_large = false;
    const auto response = http.send_stream(
      request,
      [&](std::string_view chunk) {
        if (stop.stop_requested() || clock_type::now() >= deadline) {
          return false;
        }
        if (chunk.size() > options.max_response_bytes - body.size()) {
          too_large = true;
          return false;
        }
        body.append(chunk);
        return true;
      },
      stop);
    result.http_status = response.status_code;
    if (stop.stop_requested()) {
      return failure(llm_error_code::cancelled, result.http_status);
    }
    if (too_large) {
      return failure(llm_error_code::model_list_limit_exceeded, result.http_status);
    }
    if (clock_type::now() >= deadline) {
      return failure(llm_error_code::timeout, result.http_status);
    }
    const auto error = response_error(response);
    if (error != llm_error_code::none) {
      auto transport = response.transport_error;
      if (!transport && response.error_code.category() != http_status_category()) {
        transport = response.error_code;
      }
      return failure(error, result.http_status, transport);
    }
    auto page = parse_page(body, *format);
    if (page.error != llm_error_code::none) {
      return failure(page.error, result.http_status);
    }
    for (auto& model : page.models) {
      if (ids.insert(model.id).second) {
        if (result.models.size() == options.max_models) {
          return failure(llm_error_code::model_list_limit_exceeded, result.http_status);
        }
        result.models.push_back(std::move(model));
      }
    }
    if (stop.stop_requested()) {
      return failure(llm_error_code::cancelled, result.http_status);
    }
    if (clock_type::now() >= deadline) {
      return failure(llm_error_code::timeout, result.http_status);
    }
    if (page.next_cursor.empty()) {
      return result;
    }
    if (!cursors.insert(page.next_cursor).second) {
      return failure(llm_error_code::invalid_response, result.http_status);
    }
    cursor = std::move(page.next_cursor);
  }
  return failure(llm_error_code::model_list_limit_exceeded, result.http_status);
}

WUWE_NAMESPACE_END
