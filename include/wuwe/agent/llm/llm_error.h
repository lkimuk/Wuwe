#ifndef WUWE_AGENT_LLM_ERROR_H
#define WUWE_AGENT_LLM_ERROR_H

#include <wuwe/common/wuwe_fwd.h>

WUWE_AGENT_NAMESPACE_BEGIN

enum class llm_error_code {
  none,
  transport_error,
  http_error,
  api_error,
  invalid_response,
  empty_response,
  timeout
};

WUWE_AGENT_NAMESPACE_END

#endif // WUWE_AGENT_LLM_ERROR_H