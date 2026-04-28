#ifndef WUWE_WUWE_H
#define WUWE_WUWE_H

#include <gmp/dp/object_factory.hpp>

#include <wuwe/agent/core/message.hpp>
#include <wuwe/agent/llm/llm_agent_runner.h>
#include <wuwe/agent/llm/openrouter_llm_client.h>
#include <wuwe/agent/memory/file_memory_store.hpp>
#include <wuwe/agent/memory/lexical_memory_ranker.hpp>
#include <wuwe/agent/memory/memory_context.hpp>
#include <wuwe/agent/orchestration/flow.hpp>
#include <wuwe/agent/tools/tool.hpp>
#include <wuwe/common/print.h>

WUWE_NAMESPACE_BEGIN

GMP_FACTORY_REGISTER(llm_client, llm_config, (OpenRouter, openrouter_llm_client))
using llm_client_factory = gmp::object_factory<llm_client, llm_config>;

WUWE_NAMESPACE_END

#endif // WUWE_WUWE_H
