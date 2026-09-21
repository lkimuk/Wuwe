#ifndef WUWE_AGENT_LLM_MODEL_DISCOVERY_H
#define WUWE_AGENT_LLM_MODEL_DISCOVERY_H

#include <cstddef>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <wuwe/agent/llm/llm_config.h>
#include <wuwe/agent/llm/llm_error.h>
#include <wuwe/common/wuwe_fwd.h>

WUWE_NAMESPACE_BEGIN

class http_client;

enum class llm_model_list_format { openai, anthropic, gemini, ollama };

struct llm_model_info {
  // Exact callable ID. Gemini's resource prefix "models/" is removed.
  std::string id;
  std::optional<std::string> display_name;
};

struct llm_model_list_options {
  // Defaults to the registered provider's protocol. An override also selects
  // that format's authentication headers, independently of generation.
  std::optional<llm_model_list_format> format;
  // Optional path appended to base_url (not an absolute URL or query string).
  // For example: base_url=https://gateway.test, models_path=/openai/v1/models.
  std::string models_path;
  std::size_t max_pages { 100 };
  std::size_t max_models { 10'000 };
  std::size_t max_response_bytes { 8 * 1024 * 1024 }; // Per page.
};

struct llm_model_list_result {
  // All pages, deduplicated by ID in first-seen order. Always empty on error.
  // A successful empty list is distinct from unsupported discovery.
  std::vector<llm_model_info> models;
  std::error_code error_code;
  std::error_code transport_error;
  int http_status { 0 }; // Last response, or zero before network dispatch.
};

// Stateless, synchronous discovery. Does not invoke a model or mutate config,
// registries, or routing. config.model is not required. config.timeout must be
// positive and bounds the entire operation; no automatic retries are made.
// Pagination is bounded, and failures never return a partial catalog. Network
// errors use llm_error_code; 404/405/501 mean unsupported_capability. Redirects
// are not followed. Error results never include upstream bodies or credentials.
// The transport overload borrows http only for this call. Cancellation during
// I/O requires its send_stream implementation to honor the callback/stop token.
[[nodiscard]] llm_model_list_result list_llm_models(std::string_view provider_id,
  llm_client_config config, const llm_model_list_options& options = {}, std::stop_token stop = {});

[[nodiscard]] llm_model_list_result list_llm_models(std::string_view provider_id,
  llm_client_config config, http_client& http, const llm_model_list_options& options = {},
  std::stop_token stop = {});

WUWE_NAMESPACE_END

#endif // WUWE_AGENT_LLM_MODEL_DISCOVERY_H
