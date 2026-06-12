#ifndef WUWE_AGENT_LLM_PROVIDER_REGISTRY_H
#define WUWE_AGENT_LLM_PROVIDER_REGISTRY_H

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <wuwe/agent/llm/llm_config.h>
#include <wuwe/common/wuwe_fwd.h>

WUWE_NAMESPACE_BEGIN

enum class llm_provider_protocol {
  openai_compatible,
  anthropic_messages,
  gemini_generate_content,
  ollama_chat,
};

struct llm_provider_capabilities {
  bool streaming { false };
  bool tools { false };
  bool tool_choice { false };
  bool json_response_format { false };
  bool multimodal_input { false };
  bool local_runtime { false };
};

struct llm_provider_info {
  std::string id;
  std::string display_name;
  llm_provider_protocol protocol { llm_provider_protocol::openai_compatible };
  std::string default_base_url;
  std::string default_chat_completions_path;
  bool base_url_required { false };
  bool api_key_required { true };
  std::vector<std::string> api_key_env_names;
  llm_provider_capabilities capabilities;
  std::vector<std::string> recommended_models;
};

std::string_view to_string(llm_provider_protocol protocol) noexcept;

const std::vector<llm_provider_info>& list_llm_providers();

const llm_provider_info* find_llm_provider(std::string_view id) noexcept;

std::optional<llm_client_config> make_default_llm_config(std::string_view provider_id);

llm_client_config make_default_llm_config(const llm_provider_info& provider);

std::optional<llm_client_config> normalize_llm_client_config(
  std::string_view provider_id,
  llm_client_config config);

llm_client_config normalize_llm_client_config(
  const llm_provider_info& provider,
  llm_client_config config);

WUWE_NAMESPACE_END

#endif // WUWE_AGENT_LLM_PROVIDER_REGISTRY_H
