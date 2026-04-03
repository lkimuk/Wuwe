#ifndef WUWE_AGENT_LLM_CLIENT_H
#define WUWE_AGENT_LLM_CLIENT_H

#include <wuwe/agent/llm/llm_types.h>
#include <wuwe/common/wuwe_fwd.h>

WUWE_AGENT_NAMESPACE_BEGIN

class llm_client {
public:
  virtual ~llm_client() = default;

  virtual llm_response complete(const llm_request &request) = 0;
};

WUWE_AGENT_NAMESPACE_END

#endif // WUWE_AGENT_LLM_CLIENT_H
