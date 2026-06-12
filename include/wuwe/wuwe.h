#ifndef WUWE_WUWE_H
#define WUWE_WUWE_H

#include <gmp/dp/object_factory.hpp>

#include <wuwe/agent/core/message.hpp>
#include <wuwe/agent/core/observability.hpp>
#include <wuwe/agent/knowledge/knowledge.hpp>
#include <wuwe/agent/llm/llm_agent_runner.h>
#include <wuwe/agent/llm/openrouter_llm_client.h>
#include <wuwe/agent/mcp/mcp.hpp>
#include <wuwe/agent/memory/memory.hpp>
#include <wuwe/agent/orchestration/flow.hpp>
#include <wuwe/agent/planning/planning.hpp>
#include <wuwe/agent/reasoning/reasoning.hpp>
#include <wuwe/agent/reflection/reflection.hpp>
#include <wuwe/agent/tools/tool.hpp>
#include <wuwe/common/print.h>
#include <wuwe/net/cpr_http_client.h>
#include <wuwe/net/default_http_client.h>
#include <wuwe/net/httplib_http_client.h>

WUWE_NAMESPACE_BEGIN

GMP_FACTORY_REGISTER(llm_client, llm_config, (OpenRouter, openrouter_llm_client))
using llm_client_factory = gmp::object_factory<llm_client, llm_config>;

WUWE_NAMESPACE_END

#endif // WUWE_WUWE_H
