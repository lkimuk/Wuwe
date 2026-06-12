#ifndef WUWE_AGENT_LLM_PROVIDER_FACTORY_H
#define WUWE_AGENT_LLM_PROVIDER_FACTORY_H

#include <memory>
#include <string>
#include <string_view>

#include <wuwe/agent/llm/llm_client.h>
#include <wuwe/agent/llm/llm_config.h>
#include <wuwe/common/wuwe_fwd.h>

WUWE_NAMESPACE_BEGIN

void register_builtin_llm_clients();

class llm_client_factory {
public:
  llm_client_factory();

  static llm_client_factory& instance();

  llm_client* create(std::string_view provider_id, const llm_config& config) const;

  std::shared_ptr<llm_client> create_shared(
    std::string_view provider_id,
    const llm_config& config) const;

  std::unique_ptr<llm_client> create_unique(
    std::string_view provider_id,
    const llm_config& config) const;
};

std::shared_ptr<llm_client> make_llm_client(
  std::string_view provider_id,
  llm_client_config config);

WUWE_NAMESPACE_END

#endif // WUWE_AGENT_LLM_PROVIDER_FACTORY_H
