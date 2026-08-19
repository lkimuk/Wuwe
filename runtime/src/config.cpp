#include <wuwe/runtime_host/config.hpp>

#include <fstream>
#include <regex>
#include <set>
#include <string_view>
#include <utility>

#include "endpoint_validation.hpp"

namespace wuwe::runtime_host {
namespace {

using json = nlohmann::json;

void issue(validation_report& report, std::string path, std::string code, std::string message) {
  report.issues.push_back(
    { .path = std::move(path), .code = std::move(code), .message = std::move(message) });
}

void reject_unknown_fields(const json& object, const std::set<std::string>& allowed,
  std::string_view path, validation_report& report) {
  if (!object.is_object())
    return;
  for (const auto& [key, _] : object.items()) {
    if (!allowed.contains(key)) {
      issue(report,
        std::string(path) + "/" + key,
        "unknown_field",
        "field is not part of runtime config schema version 1");
    }
  }
}

const json* object_field(const json& parent, const char* name, std::string_view path, bool required,
  validation_report& report) {
  const auto found = parent.find(name);
  if (found == parent.end()) {
    if (required) {
      issue(report, std::string(path) + "/" + name, "required", "required object is missing");
    }
    return nullptr;
  }
  if (!found->is_object()) {
    issue(report, std::string(path) + "/" + name, "invalid_type", "value must be an object");
    return nullptr;
  }
  return &*found;
}

void validate_positive_integer(const json& object, const char* name, std::string_view path,
  std::int64_t minimum, std::int64_t maximum, validation_report& report) {
  const auto found = object.find(name);
  if (found == object.end())
    return;
  if (!found->is_number_integer()) {
    issue(report, std::string(path) + "/" + name, "invalid_type", "value must be an integer");
    return;
  }
  const auto value = found->get<std::int64_t>();
  if (value < minimum || value > maximum) {
    issue(report,
      std::string(path) + "/" + name,
      "out_of_range",
      "value must be between " + std::to_string(minimum) + " and " + std::to_string(maximum));
  }
}

void validate_optional_string(
  const json& object, const char* name, std::string_view path, validation_report& report) {
  const auto found = object.find(name);
  if (found != object.end() && !found->is_string()) {
    issue(report, std::string(path) + "/" + name, "invalid_type", "value must be a string");
  }
}

std::string safe_string(const json& object, const char* name, std::string fallback = {}) {
  const auto found = object.find(name);
  return found != object.end() && found->is_string() ? found->get<std::string>()
                                                     : std::move(fallback);
}

std::chrono::milliseconds milliseconds_value(
  const json& object, const char* name, std::chrono::milliseconds fallback) {
  const auto found = object.find(name);
  return found == object.end() ? fallback : std::chrono::milliseconds(found->get<std::int64_t>());
}

void validate_base_url(std::string_view value, validation_report& report) {
  if (value.empty())
    return;
  const auto scheme_size = value.starts_with("https://")  ? std::size_t { 8 }
                           : value.starts_with("http://") ? std::size_t { 7 }
                                                          : std::size_t {};
  if (scheme_size == 0) {
    issue(report, "/llm/baseUrl", "invalid_format", "baseUrl must use http:// or https://");
    return;
  }
  const auto authority_end = value.find_first_of("/?#", scheme_size);
  const auto authority = value.substr(scheme_size,
    authority_end == value.npos ? value.size() - scheme_size : authority_end - scheme_size);
  if (authority.empty() || authority.find('@') != authority.npos) {
    issue(report,
      "/llm/baseUrl",
      authority.empty() ? "invalid_format" : "inline_secret",
      authority.empty() ? "baseUrl must include a host"
                        : "baseUrl must not contain embedded credentials");
  }
  else if (!detail::valid_network_endpoint(authority)) {
    issue(report,
      "/llm/baseUrl",
      "invalid_format",
      "baseUrl must contain a valid host and optional port");
  }
  if (value.find_first_of("\r\n\t ") != value.npos || value.find('\\') != value.npos ||
      value.find('?') != value.npos || value.find('#') != value.npos) {
    issue(report,
      "/llm/baseUrl",
      "invalid_format",
      "baseUrl must not contain whitespace, backslashes, a query, or a fragment");
  }
}

} // namespace

std::string to_string(run_store_kind kind) {
  switch (kind) {
    case run_store_kind::memory:
      return "memory";
    case run_store_kind::sqlite:
      return "sqlite";
  }
  return "unknown";
}

std::string to_string(serve_transport transport) {
  switch (transport) {
    case serve_transport::stdio:
      return "stdio";
  }
  return "unknown";
}

validation_report validate_runtime_config(const nlohmann::json& document) {
  validation_report report;
  if (!document.is_object()) {
    issue(report, "/", "invalid_type", "runtime configuration must be a JSON object");
    return report;
  }

  reject_unknown_fields(
    document, { "schemaVersion", "runtime", "llm", "store", "serve" }, "", report);

  const auto schema = document.find("schemaVersion");
  if (schema == document.end()) {
    issue(report, "/schemaVersion", "required", "schemaVersion is required");
  }
  else if (!schema->is_number_integer()) {
    issue(report, "/schemaVersion", "invalid_type", "schemaVersion must be an integer");
  }
  else if (schema->get<std::int64_t>() != runtime_config_schema_version) {
    issue(report,
      "/schemaVersion",
      "unsupported_version",
      "supported runtime config schema version is 1");
  }

  if (const auto* runtime = object_field(document, "runtime", "", false, report)) {
    reject_unknown_fields(*runtime,
      { "maxConcurrentRuns", "defaultRunTimeoutMs", "submissionTimeoutMs", "shutdownTimeoutMs" },
      "/runtime",
      report);
    validate_positive_integer(*runtime, "maxConcurrentRuns", "/runtime", 1, 1024, report);
    validate_positive_integer(*runtime, "defaultRunTimeoutMs", "/runtime", 1, 86400000, report);
    validate_positive_integer(*runtime, "submissionTimeoutMs", "/runtime", 1, 600000, report);
    validate_positive_integer(*runtime, "shutdownTimeoutMs", "/runtime", 1, 600000, report);
  }

  if (const auto* llm = object_field(document, "llm", "", true, report)) {
    reject_unknown_fields(*llm,
      { "provider",
        "model",
        "apiKeyEnv",
        "baseUrl",
        "chatCompletionsPath",
        "timeoutMs",
        "maxRetries" },
      "/llm",
      report);
    for (const auto* required : { "provider", "model" }) {
      const auto found = llm->find(required);
      if (found == llm->end() || !found->is_string() ||
          found->get_ref<const std::string&>().empty()) {
        issue(
          report, std::string("/llm/") + required, "required", "value must be a non-empty string");
      }
    }
    validate_optional_string(*llm, "apiKeyEnv", "/llm", report);
    validate_optional_string(*llm, "baseUrl", "/llm", report);
    validate_optional_string(*llm, "chatCompletionsPath", "/llm", report);
    validate_positive_integer(*llm, "timeoutMs", "/llm", 1, 600000, report);
    validate_positive_integer(*llm, "maxRetries", "/llm", 0, 20, report);

    const auto provider = safe_string(*llm, "provider");
    static const std::regex provider_pattern("^[A-Za-z0-9][A-Za-z0-9._-]*$");
    if (!provider.empty() && !std::regex_match(provider, provider_pattern)) {
      issue(report,
        "/llm/provider",
        "invalid_format",
        "provider must contain only letters, digits, '.', '_' or '-' and start with a letter or "
        "digit");
    }
    const auto api_key_env = safe_string(*llm, "apiKeyEnv");
    static const std::regex env_pattern("^[A-Za-z_][A-Za-z0-9_]*$");
    if (!api_key_env.empty() && !std::regex_match(api_key_env, env_pattern)) {
      issue(report,
        "/llm/apiKeyEnv",
        "invalid_format",
        "apiKeyEnv must be a valid environment variable name");
    }
    const auto base_url = safe_string(*llm, "baseUrl");
    validate_base_url(base_url, report);
  }

  if (const auto* store = object_field(document, "store", "", false, report)) {
    reject_unknown_fields(*store, { "kind", "path" }, "/store", report);
    const auto kind = store->find("kind");
    if (kind != store->end() && !kind->is_string()) {
      issue(report, "/store/kind", "invalid_type", "kind must be a string");
    }
    const auto kind_value = safe_string(*store, "kind", "memory");
    if (kind_value != "memory" && kind_value != "sqlite") {
      issue(report, "/store/kind", "unsupported_value", "kind must be 'memory' or 'sqlite'");
    }
    validate_optional_string(*store, "path", "/store", report);
    const auto path = safe_string(*store, "path");
    if (kind_value == "sqlite" && path.empty()) {
      issue(report, "/store/path", "required", "path is required for a SQLite run store");
    }
    if (kind_value == "memory" && !path.empty()) {
      issue(report, "/store/path", "not_applicable", "path is only valid for a SQLite run store");
    }
  }

  if (const auto* serve = object_field(document, "serve", "", false, report)) {
    reject_unknown_fields(*serve, { "transport" }, "/serve", report);
    const auto transport = serve->find("transport");
    if (transport != serve->end() && !transport->is_string()) {
      issue(report, "/serve/transport", "invalid_type", "transport must be a string");
    }
    else if (safe_string(*serve, "transport", "stdio") != "stdio") {
      issue(report,
        "/serve/transport",
        "unsupported_value",
        "runtime version 1 supports the 'stdio' transport");
    }
  }

  const auto runtime_document = document.find("runtime");
  const auto llm_document = document.find("llm");
  if (llm_document != document.end() && llm_document->is_object()) {
    std::int64_t run_timeout = 120000;
    std::int64_t llm_timeout = 30000;
    if (runtime_document != document.end() && runtime_document->is_object()) {
      const auto configured = runtime_document->find("defaultRunTimeoutMs");
      if (configured != runtime_document->end() && configured->is_number_integer())
        run_timeout = configured->get<std::int64_t>();
    }
    const auto configured = llm_document->find("timeoutMs");
    if (configured != llm_document->end() && configured->is_number_integer())
      llm_timeout = configured->get<std::int64_t>();
    if (run_timeout > 0 && llm_timeout > run_timeout) {
      issue(report,
        "/llm/timeoutMs",
        "inconsistent_timeout",
        "LLM request timeout must not exceed the runtime run timeout");
    }
  }
  return report;
}

runtime_config runtime_config_from_json(const nlohmann::json& document) {
  auto report = validate_runtime_config(document);
  if (!report)
    throw validation_error("runtime configuration", std::move(report));

  runtime_config config;
  config.schema_version = document.at("schemaVersion").get<int>();
  if (const auto found = document.find("runtime"); found != document.end()) {
    config.runtime.max_concurrent_runs =
      found->value("maxConcurrentRuns", config.runtime.max_concurrent_runs);
    config.runtime.default_run_timeout =
      milliseconds_value(*found, "defaultRunTimeoutMs", config.runtime.default_run_timeout);
    config.runtime.submission_timeout =
      milliseconds_value(*found, "submissionTimeoutMs", config.runtime.submission_timeout);
    config.runtime.shutdown_timeout =
      milliseconds_value(*found, "shutdownTimeoutMs", config.runtime.shutdown_timeout);
  }
  const auto& llm = document.at("llm");
  config.llm.provider = llm.at("provider").get<std::string>();
  config.llm.model = llm.at("model").get<std::string>();
  config.llm.api_key_env = llm.value("apiKeyEnv", std::string {});
  config.llm.base_url = llm.value("baseUrl", std::string {});
  config.llm.chat_completions_path = llm.value("chatCompletionsPath", std::string {});
  config.llm.timeout = milliseconds_value(llm, "timeoutMs", config.llm.timeout);
  config.llm.max_retries = llm.value("maxRetries", config.llm.max_retries);

  if (const auto found = document.find("store"); found != document.end()) {
    config.store.kind = found->value("kind", std::string("memory")) == "sqlite"
                          ? run_store_kind::sqlite
                          : run_store_kind::memory;
    config.store.sqlite_path = found->value("path", std::string {});
  }
  return config;
}

runtime_config load_runtime_config(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input)
    throw std::runtime_error("unable to open runtime config: " + path.string());
  try {
    nlohmann::json document;
    input >> document;
    return runtime_config_from_json(document);
  }
  catch (const nlohmann::json::exception& ex) {
    throw std::runtime_error(
      "unable to parse runtime config '" + path.string() + "': " + ex.what());
  }
}

nlohmann::json runtime_config_to_json(const runtime_config& config) {
  nlohmann::json store {
    { "kind", to_string(config.store.kind) },
  };
  if (!config.store.sqlite_path.empty())
    store["path"] = config.store.sqlite_path.generic_string();
  return {
    { "schemaVersion", config.schema_version },
    { "runtime",
      {
        { "maxConcurrentRuns", config.runtime.max_concurrent_runs },
        { "defaultRunTimeoutMs", config.runtime.default_run_timeout.count() },
        { "submissionTimeoutMs", config.runtime.submission_timeout.count() },
        { "shutdownTimeoutMs", config.runtime.shutdown_timeout.count() },
      } },
    { "llm",
      {
        { "provider", config.llm.provider },
        { "model", config.llm.model },
        { "apiKeyEnv", config.llm.api_key_env },
        { "baseUrl", config.llm.base_url },
        { "chatCompletionsPath", config.llm.chat_completions_path },
        { "timeoutMs", config.llm.timeout.count() },
        { "maxRetries", config.llm.max_retries },
      } },
    { "store", std::move(store) },
    { "serve", { { "transport", to_string(config.serve.transport) } } },
  };
}

} // namespace wuwe::runtime_host
