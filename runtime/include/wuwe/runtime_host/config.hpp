#ifndef WUWE_RUNTIME_HOST_CONFIG_HPP
#define WUWE_RUNTIME_HOST_CONFIG_HPP

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include <wuwe/runtime_host/validation.hpp>

namespace wuwe::runtime_host {

inline constexpr int runtime_config_schema_version = 1;

enum class run_store_kind {
  memory,
  sqlite,
};

enum class serve_transport {
  stdio,
};

struct runtime_options {
  std::size_t max_concurrent_runs { 4 };
  std::chrono::milliseconds default_run_timeout { 120000 };
  std::chrono::milliseconds submission_timeout { 5000 };
  std::chrono::milliseconds shutdown_timeout { 10000 };
};

struct llm_options {
  std::string provider;
  std::string model;
  std::string api_key_env;
  std::string base_url;
  std::string chat_completions_path;
  std::chrono::milliseconds timeout { 30000 };
  int max_retries { 2 };
};

struct run_store_options {
  run_store_kind kind { run_store_kind::memory };
  std::filesystem::path sqlite_path;
};

struct serve_options {
  serve_transport transport { serve_transport::stdio };
};

struct runtime_config {
  int schema_version { runtime_config_schema_version };
  runtime_options runtime;
  llm_options llm;
  run_store_options store;
  serve_options serve;
};

[[nodiscard]] std::string to_string(run_store_kind kind);
[[nodiscard]] std::string to_string(serve_transport transport);

[[nodiscard]] validation_report validate_runtime_config(const nlohmann::json& document);
[[nodiscard]] runtime_config runtime_config_from_json(const nlohmann::json& document);
[[nodiscard]] runtime_config load_runtime_config(const std::filesystem::path& path);
[[nodiscard]] nlohmann::json runtime_config_to_json(const runtime_config& config);

} // namespace wuwe::runtime_host

#endif // WUWE_RUNTIME_HOST_CONFIG_HPP
