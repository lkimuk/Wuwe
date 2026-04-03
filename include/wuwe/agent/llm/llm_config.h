#ifndef WUWE_AGENT_LLM_CONFIG_H
#define WUWE_AGENT_LLM_CONFIG_H

#include <string>

#include <wuwe/common/wuwe_fwd.h>

WUWE_AGENT_NAMESPACE_BEGIN

struct llm_client_config {
  std::string base_url;
  std::string api_key;
  std::string default_model;
  int timeout_ms{30000};
};

WUWE_AGENT_NAMESPACE_END

#endif // WUWE_AGENT_LLM_CONFIG_H
