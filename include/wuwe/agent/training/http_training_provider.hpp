#ifndef WUWE_AGENT_TRAINING_HTTP_TRAINING_PROVIDER_HPP
#define WUWE_AGENT_TRAINING_HTTP_TRAINING_PROVIDER_HPP

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include <wuwe/agent/training/training_codec.hpp>
#include <wuwe/agent/training/training_provider.hpp>
#include <wuwe/net/default_http_client.h>
#include <wuwe/net/http_client.h>
#include <wuwe/net/transport_error.h>

namespace wuwe::agent::training {

inline constexpr std::string_view training_protocol_version = "2026-08-01";

struct http_training_provider_config {
  std::string name { "http" };
  std::string base_url;
  std::string bearer_token;
  int timeout_ms { 30000 };
  std::size_t max_response_bytes { 4 * 1024 * 1024 };
  bool allow_insecure_transport { false };
  std::map<std::string, std::string> headers;
};

class http_training_provider final : public training_provider {
public:
  http_training_provider(http_training_provider_config config,
    std::shared_ptr<::wuwe::http_client> http = std::make_shared<::wuwe::default_http_client>())
      : config_(std::move(config)), http_(std::move(http)) {
    while (config_.base_url.size() > 1 && config_.base_url.back() == '/')
      config_.base_url.pop_back();
    if (config_.name.empty() || config_.base_url.empty() || config_.timeout_ms <= 0 ||
        config_.max_response_bytes == 0) {
      throw std::invalid_argument(
        "HTTP training provider requires name, base URL, timeout, and response size limit");
    }
    if (!http_)
      throw std::invalid_argument("HTTP training provider requires an HTTP client");
    if (!valid_header_value(config_.base_url) || !valid_header_value(config_.bearer_token)) {
      throw std::invalid_argument(
        "HTTP training provider configuration contains control characters");
    }
    const auto secure_transport = starts_with_case_insensitive(config_.base_url, "https://");
    const auto insecure_transport = starts_with_case_insensitive(config_.base_url, "http://");
    if (!secure_transport && !(config_.allow_insecure_transport && insecure_transport)) {
      throw std::invalid_argument(
        "HTTP training provider requires HTTPS unless insecure transport is explicitly enabled");
    }
    for (const auto& [name, value] : config_.headers) {
      if (!valid_header_name(name) || !valid_header_value(value))
        throw std::invalid_argument("HTTP training provider contains an invalid custom header");
      if (reserved_header(name))
        throw std::invalid_argument(
          "HTTP training provider custom headers must not override protocol or identity headers");
    }
  }

  [[nodiscard]] std::string name() const override {
    return config_.name;
  }

  [[nodiscard]] training_result<training_provider_snapshot> submit(
    const training_request& request, const training_context& context) override {
    try {
      validate_training_request(request);
    }
    catch (const std::exception& ex) {
      return failure(
        training_errc::invalid_request, exception_message(ex, "invalid HTTP training request"));
    }
    return send("POST",
      "/v1/training/jobs",
      training_request_to_json(request),
      context,
      request.idempotency_key);
  }

  [[nodiscard]] training_result<training_provider_snapshot> inspect(
    const std::string& provider_job_id, const training_context& context) override {
    if (provider_job_id.empty())
      return failure(
        training_errc::invalid_request, "HTTP training provider job id must not be empty");
    return send("GET", job_path(provider_job_id), {}, context);
  }

  [[nodiscard]] training_result<std::optional<training_submission_record>> lookup_submission(
    const std::string& idempotency_key, const training_context& context) override {
    if (idempotency_key.empty())
      return lookup_failure(
        training_errc::invalid_request, "HTTP training provider idempotency key must not be empty");
    auto result = send_typed<training_submission_record>("GET",
      "/v1/training/submissions/" + url_encode(idempotency_key),
      {},
      context,
      idempotency_key,
      "lookup",
      [](const nlohmann::json& body) { return training_submission_record_from_json(body); });
    if (const auto* error = result.error_if()) {
      if (error->code == training_errc::not_found)
        return training_result<std::optional<training_submission_record>>::success(std::nullopt);
      return training_result<std::optional<training_submission_record>>::failure(*error);
    }
    return training_result<std::optional<training_submission_record>>::success(*result.value_if());
  }

  [[nodiscard]] training_result<training_provider_snapshot> cancel(
    const std::string& provider_job_id, const training_context& context) override {
    if (provider_job_id.empty())
      return failure(
        training_errc::invalid_request, "HTTP training provider job id must not be empty");
    return send("POST", job_path(provider_job_id) + "/cancel", nlohmann::json::object(), context);
  }

  [[nodiscard]] training_result<training_provider_snapshot> resume(
    const std::string& provider_job_id, const training_checkpoint& checkpoint,
    const training_context& context) override {
    if (provider_job_id.empty())
      return failure(
        training_errc::invalid_request, "HTTP training provider job id must not be empty");
    try {
      validate_training_checkpoint(checkpoint);
    }
    catch (const std::exception& ex) {
      return failure(
        training_errc::invalid_request, exception_message(ex, "invalid HTTP training checkpoint"));
    }
    return send("POST",
      job_path(provider_job_id) + "/resume",
      { { "checkpoint", training_checkpoint_to_json(checkpoint) } },
      context);
  }

private:
  [[nodiscard]] training_result<training_provider_snapshot> send(const std::string& method,
    const std::string& path, const nlohmann::json& body, const training_context& context,
    const std::string& idempotency_key = {}, const std::string& explicit_operation = {}) const {
    return send_typed<training_provider_snapshot>(method,
      path,
      body,
      context,
      idempotency_key,
      explicit_operation,
      [](const nlohmann::json& response_body) {
        return training_provider_snapshot_from_json(response_body);
      });
  }

  template<typename T, typename Parser>
  [[nodiscard]] training_result<T> send_typed(const std::string& method, const std::string& path,
    const nlohmann::json& body, const training_context& context, const std::string& idempotency_key,
    const std::string& explicit_operation, Parser&& parse_success) const {
    const auto operation =
      explicit_operation.empty() ? operation_for(method, path) : explicit_operation;
    const auto mutating = operation == "submit" || operation == "cancel" || operation == "resume";
    const auto effect =
      mutating ? training_remote_effect::mutation : training_remote_effect::read_only;
    const auto not_sent_outcome =
      mutating ? training_remote_outcome::not_applied : training_remote_outcome::not_applicable;
    if (context.cancellation_requested())
      return typed_failure<T>(training_errc::cancelled,
        "HTTP training provider request cancelled",
        false,
        nlohmann::json::object(),
        not_sent_outcome);
    if (context.deadline_reached())
      return typed_failure<T>(training_errc::timed_out,
        "HTTP training provider request timed out",
        false,
        nlohmann::json::object(),
        not_sent_outcome);
    if (!valid_header_value(context.request_id) || !valid_header_value(context.trace_id) ||
        !valid_header_value(context.subject_id) || !valid_header_value(context.tenant_id) ||
        !valid_header_value(context.workspace_id) || !valid_header_value(idempotency_key)) {
      return typed_failure<T>(training_errc::invalid_request,
        "HTTP training provider identity and idempotency values contain control characters",
        false,
        nlohmann::json::object(),
        not_sent_outcome);
    }

    ::wuwe::http_request request {
      .method = method,
      .url = config_.base_url + path,
      .headers = { { "Accept", "application/json" } },
      .timeout = effective_timeout(context),
      .follow_redirects = false,
      .max_redirects = 0,
    };
    const auto request_id =
      context.request_id.empty() ? make_training_id("request") : context.request_id;
    request.headers.push_back({ "Wuwe-Training-Protocol", std::string(training_protocol_version) });
    request.headers.push_back({ "Wuwe-Request-Id", request_id });
    request.headers.push_back({ "Wuwe-Training-Operation", operation });
    add_context_header(request, "Wuwe-Trace-Id", context.trace_id);
    add_context_header(request, "Wuwe-Subject-Id", context.subject_id);
    add_context_header(request, "Wuwe-Tenant-Id", context.tenant_id);
    add_context_header(request, "Wuwe-Workspace-Id", context.workspace_id);
    if (method != "GET") {
      request.headers.push_back({ "Content-Type", "application/json" });
      request.body =
        nlohmann::json {
          { "protocolVersion", training_protocol_version },
          { "requestId", request_id },
          { "operation", operation },
          { "context",
            {
              { "traceId", context.trace_id },
              { "subjectId", context.subject_id },
              { "tenantId", context.tenant_id },
              { "workspaceId", context.workspace_id },
            } },
          { "body", body },
        }
          .dump();
    }
    if (!config_.bearer_token.empty())
      request.headers.push_back({ "Authorization", "Bearer " + config_.bearer_token });
    if (!idempotency_key.empty())
      request.headers.push_back({ "Idempotency-Key", idempotency_key });
    for (const auto& [name, value] : config_.headers)
      request.headers.push_back({ name, value });

    ::wuwe::http_response response;
    std::size_t response_bytes {};
    bool response_too_large {};
    try {
      response = http_->send_stream(
        request,
        [&](std::string_view chunk) {
          if (chunk.size() > config_.max_response_bytes - response_bytes) {
            response_too_large = true;
            return false;
          }
          response_bytes += chunk.size();
          return true;
        },
        context.stop_token);
    }
    catch (const std::exception& ex) {
      return typed_failure<T>(training_errc::provider_transport,
        exception_message(ex, "HTTP training provider transport failed"),
        true,
        nlohmann::json::object(),
        mutating ? training_remote_outcome::uncertain : training_remote_outcome::not_applicable);
    }
    catch (...) {
      return typed_failure<T>(training_errc::provider_transport,
        "HTTP training provider failed with an unknown exception",
        true,
        nlohmann::json::object(),
        mutating ? training_remote_outcome::uncertain : training_remote_outcome::not_applicable);
    }
    if (response_too_large) {
      return typed_failure<T>(training_errc::provider_protocol,
        "HTTP training provider response exceeds the configured size limit",
        false,
        nlohmann::json::object(),
        mutating ? training_remote_outcome::uncertain : training_remote_outcome::not_applicable);
    }
    if (response.transport_error || (response.status_code == 0 && response.error_code)) {
      const auto& error = response.transport_error ? response.transport_error : response.error_code;
      if (context.cancellation_requested() ||
          error == std::make_error_code(std::errc::operation_canceled)) {
        return typed_failure<T>(training_errc::cancelled,
          "HTTP training provider request cancelled during transport",
          false,
          nlohmann::json::object(),
          mutating ? training_remote_outcome::uncertain : training_remote_outcome::not_applicable);
      }
      if (error.category() == ::wuwe::transport_error_category() &&
          ::wuwe::is_timeout(static_cast<::wuwe::transport_error>(error.value()))) {
        return typed_failure<T>(training_errc::timed_out,
          "HTTP training provider request timed out during transport",
          true,
          nlohmann::json::object(),
          mutating ? training_remote_outcome::uncertain : training_remote_outcome::not_applicable);
      }
      auto message = error.message();
      if (message.empty())
        message = "unknown transport error";
      return typed_failure<T>(training_errc::provider_transport,
        "HTTP training provider transport failed: " + message,
        true,
        nlohmann::json::object(),
        mutating ? training_remote_outcome::uncertain : training_remote_outcome::not_applicable);
    }
    const auto ambiguous_response_failure = [&](training_errc code, std::string message) {
      return typed_failure<T>(code,
        std::move(message),
        false,
        nlohmann::json::object(),
        mutating ? training_remote_outcome::uncertain : training_remote_outcome::not_applicable);
    };
    const auto value = nlohmann::json::parse(response.body, nullptr, false);
    if (value.is_discarded())
      return ambiguous_response_failure(
        training_errc::provider_protocol, "HTTP training provider returned invalid JSON");
    if (!value.is_object() ||
        value.value("protocolVersion", std::string {}) != training_protocol_version ||
        !value.contains("ok") || !value.at("ok").is_boolean()) {
      return ambiguous_response_failure(training_errc::provider_protocol,
        "HTTP training provider returned an invalid response envelope");
    }
    if (value.value("requestId", std::string {}) != request_id ||
        value.value("operation", std::string {}) != operation) {
      return ambiguous_response_failure(training_errc::provider_protocol,
        "HTTP training provider response does not match the request envelope");
    }
    const auto ok = value.at("ok").get<bool>();
    const auto http_success = response.status_code >= 200 && response.status_code < 300;
    if (ok != http_success) {
      return ambiguous_response_failure(training_errc::provider_protocol,
        "HTTP status and training response success flag are inconsistent");
    }
    const auto has_body = value.contains("body") && !value.at("body").is_null();
    const auto has_error = value.contains("error") && !value.at("error").is_null();
    if (ok && has_error)
      return ambiguous_response_failure(training_errc::provider_protocol,
        "HTTP training provider success response must not contain an error");
    if (!ok && has_body)
      return ambiguous_response_failure(training_errc::provider_protocol,
        "HTTP training provider error response must not contain a business body");
    if (!ok) {
      if (!value.contains("error") || !value.at("error").is_object()) {
        return ambiguous_response_failure(training_errc::provider_protocol,
          "HTTP training provider error response requires a typed error envelope");
      }
      const auto& error = value.at("error");
      if (!error.contains("code") || !error.at("code").is_string() ||
          error.at("code").get<std::string>().empty() || !error.contains("message") ||
          !error.at("message").is_string() || error.at("message").get<std::string>().empty()) {
        return ambiguous_response_failure(training_errc::provider_protocol,
          "HTTP training provider error requires non-empty code and message");
      }
      if (error.contains("retryable") && !error.at("retryable").is_boolean())
        return ambiguous_response_failure(training_errc::provider_protocol,
          "HTTP training provider error retryable field must be boolean");
      if (error.contains("details") && !error.at("details").is_object())
        return ambiguous_response_failure(training_errc::provider_protocol,
          "HTTP training provider error details must be an object");
      if (error.contains("remoteOutcome") && !error.at("remoteOutcome").is_string())
        return ambiguous_response_failure(training_errc::provider_protocol,
          "HTTP training provider error remoteOutcome field must be a string");
      const auto code = training_errc_from_string(error.at("code").get<std::string>());
      if (!code)
        return ambiguous_response_failure(training_errc::provider_protocol,
          "HTTP training provider returned an unknown typed error code");
      auto remote_outcome =
        mutating ? (ambiguous_remote_error(*code) ? training_remote_outcome::uncertain
                                                  : training_remote_outcome::not_applied)
                 : training_remote_outcome::not_applicable;
      if (error.contains("remoteOutcome")) {
        const auto parsed =
          training_remote_outcome_from_string(error.at("remoteOutcome").get<std::string>());
        if (!parsed)
          return ambiguous_response_failure(training_errc::provider_protocol,
            "HTTP training provider returned an unknown remoteOutcome value");
        remote_outcome = *parsed;
      }
      training_error decoded {
        .code = *code,
        .message = error.at("message").get<std::string>(),
        .retryable = error.value("retryable", false),
        .details = error.value("details", nlohmann::json::object()),
        .remote_outcome = remote_outcome,
      };
      try {
        validate_training_error(decoded, effect);
      }
      catch (const std::exception&) {
        return ambiguous_response_failure(training_errc::provider_protocol,
          "HTTP training provider error code, operation, and remoteOutcome are inconsistent");
      }
      return training_result<T>::failure(std::move(decoded));
    }
    if (!has_body || !value.at("body").is_object())
      return ambiguous_response_failure(training_errc::provider_protocol,
        "HTTP training provider success response requires an object body");
    try {
      return training_result<T>::success(std::forward<Parser>(parse_success)(value.at("body")));
    }
    catch (const std::exception& ex) {
      auto details = nlohmann::json::object();
      if (operation == "lookup") {
        details["submission_found"] = true;
        const auto& response_body = value.at("body");
        if (response_body.contains("snapshot") && response_body.at("snapshot").is_object()) {
          const auto& snapshot = response_body.at("snapshot");
          if (snapshot.contains("provider_job_id") && snapshot.at("provider_job_id").is_string()) {
            details["provider_job_id"] = snapshot.at("provider_job_id");
          }
        }
      }
      return typed_failure<T>(training_errc::provider_protocol,
        exception_message(ex, "HTTP training provider returned an invalid success body"),
        false,
        std::move(details),
        mutating ? training_remote_outcome::applied : training_remote_outcome::not_applicable);
    }
    catch (...) {
      auto details = nlohmann::json::object();
      if (operation == "lookup")
        details["submission_found"] = true;
      return typed_failure<T>(training_errc::provider_protocol,
        "HTTP training provider success body parser failed with an unknown exception",
        false,
        std::move(details),
        mutating ? training_remote_outcome::applied : training_remote_outcome::not_applicable);
    }
  }

  static void add_context_header(
    ::wuwe::http_request& request, std::string name, const std::string& value) {
    if (!value.empty())
      request.headers.push_back({ std::move(name), value });
  }

  [[nodiscard]] static bool valid_header_value(std::string_view value) noexcept {
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
      return ch == '\t' || (ch >= 0x20 && ch != 0x7f);
    });
  }

  [[nodiscard]] static bool valid_header_name(std::string_view value) noexcept {
    if (value.empty())
      return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
      if (std::isalnum(ch))
        return true;
      switch (ch) {
        case '!':
        case '#':
        case '$':
        case '%':
        case '&':
        case '\'':
        case '*':
        case '+':
        case '-':
        case '.':
        case '^':
        case '_':
        case '`':
        case '|':
        case '~':
          return true;
        default:
          return false;
      }
    });
  }

  [[nodiscard]] static bool starts_with_case_insensitive(
    std::string_view value, std::string_view prefix) noexcept {
    return value.size() >= prefix.size() &&
           header_name_equals(value.substr(0, prefix.size()), prefix);
  }

  [[nodiscard]] static bool header_name_equals(
    std::string_view lhs, std::string_view rhs) noexcept {
    return lhs.size() == rhs.size() &&
           std::equal(lhs.begin(), lhs.end(), rhs.begin(), [](char left, char right) {
             return std::tolower(static_cast<unsigned char>(left)) ==
                    std::tolower(static_cast<unsigned char>(right));
           });
  }

  [[nodiscard]] static bool reserved_header(std::string_view name) noexcept {
    constexpr std::string_view reserved[] {
      "Accept",
      "Content-Type",
      "Authorization",
      "Proxy-Authorization",
      "Host",
      "Content-Length",
      "Transfer-Encoding",
      "Connection",
      "TE",
      "Trailer",
      "Upgrade",
      "Expect",
      "Idempotency-Key",
      "Wuwe-Training-Protocol",
      "Wuwe-Request-Id",
      "Wuwe-Training-Operation",
      "Wuwe-Trace-Id",
      "Wuwe-Subject-Id",
      "Wuwe-Tenant-Id",
      "Wuwe-Workspace-Id",
    };
    return std::any_of(std::begin(reserved), std::end(reserved), [&](std::string_view candidate) {
      return header_name_equals(name, candidate);
    });
  }

  [[nodiscard]] int effective_timeout(const training_context& context) const noexcept {
    if (!context.deadline)
      return config_.timeout_ms;
    const auto remaining = context.remaining_time().count();
    if (remaining <= 0)
      return 1;
    return static_cast<int>(
      (std::min)(static_cast<long long>(config_.timeout_ms), static_cast<long long>(remaining)));
  }

  static std::string job_path(const std::string& provider_job_id) {
    if (provider_job_id.empty())
      throw std::invalid_argument("HTTP training provider job id must not be empty");
    return "/v1/training/jobs/" + url_encode(provider_job_id);
  }

  static std::string url_encode(std::string_view value) {
    std::string encoded;
    constexpr char hex[] = "0123456789ABCDEF";
    for (const unsigned char ch : value) {
      if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
        encoded.push_back(static_cast<char>(ch));
      }
      else {
        encoded.push_back('%');
        encoded.push_back(hex[ch >> 4]);
        encoded.push_back(hex[ch & 0x0f]);
      }
    }
    return encoded;
  }

  [[nodiscard]] static std::string operation_for(
    const std::string& method, const std::string& path) {
    if (method == "GET")
      return "inspect";
    if (path.ends_with("/cancel"))
      return "cancel";
    if (path.ends_with("/resume"))
      return "resume";
    return "submit";
  }

  [[nodiscard]] static std::optional<training_errc> training_errc_from_string(
    std::string_view value) {
    if (value == "invalid_request")
      return training_errc::invalid_request;
    if (value == "not_found")
      return training_errc::not_found;
    if (value == "invalid_transition")
      return training_errc::invalid_transition;
    if (value == "revision_conflict")
      return training_errc::revision_conflict;
    if (value == "provider_transport")
      return training_errc::provider_transport;
    if (value == "provider_protocol")
      return training_errc::provider_protocol;
    if (value == "authorization_denied")
      return training_errc::authorization_denied;
    if (value == "persistence_failure")
      return training_errc::persistence_failure;
    if (value == "corrupted_state")
      return training_errc::corrupted_state;
    if (value == "cancelled")
      return training_errc::cancelled;
    if (value == "timed_out")
      return training_errc::timed_out;
    if (value == "unsupported_operation")
      return training_errc::unsupported_operation;
    if (value == "reconciliation_required")
      return training_errc::reconciliation_required;
    return std::nullopt;
  }

  [[nodiscard]] static std::optional<training_remote_outcome> training_remote_outcome_from_string(
    std::string_view value) noexcept {
    if (value == "not_applicable")
      return training_remote_outcome::not_applicable;
    if (value == "not_applied")
      return training_remote_outcome::not_applied;
    if (value == "applied")
      return training_remote_outcome::applied;
    if (value == "uncertain")
      return training_remote_outcome::uncertain;
    return std::nullopt;
  }

  [[nodiscard]] static bool ambiguous_remote_error(training_errc code) noexcept {
    return ambiguous_training_error(code);
  }

  [[nodiscard]] static std::string exception_message(
    const std::exception& ex, std::string fallback) {
    const std::string message = ex.what();
    return message.empty() ? std::move(fallback) : message;
  }

  template<typename T>
  [[nodiscard]] static training_result<T> typed_failure(training_errc code, std::string message,
    bool retryable = false, nlohmann::json details = nlohmann::json::object(),
    training_remote_outcome remote_outcome = training_remote_outcome::not_applicable) {
    return training_result<T>::failure({
      .code = code,
      .message = std::move(message),
      .retryable = retryable,
      .details = std::move(details),
      .remote_outcome = remote_outcome,
    });
  }

  [[nodiscard]] static training_result<training_provider_snapshot> failure(training_errc code,
    std::string message, bool retryable = false, nlohmann::json details = nlohmann::json::object(),
    training_remote_outcome remote_outcome = training_remote_outcome::not_applicable) {
    return typed_failure<training_provider_snapshot>(
      code, std::move(message), retryable, std::move(details), remote_outcome);
  }

  [[nodiscard]] static training_result<std::optional<training_submission_record>> lookup_failure(
    training_errc code, std::string message) {
    return training_result<std::optional<training_submission_record>>::failure({
      .code = code,
      .message = std::move(message),
    });
  }

  http_training_provider_config config_;
  std::shared_ptr<::wuwe::http_client> http_;
};

} // namespace wuwe::agent::training

#endif // WUWE_AGENT_TRAINING_HTTP_TRAINING_PROVIDER_HPP
