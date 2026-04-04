#ifndef WUWE_WUWE_H
#define WUWE_WUWE_H

#include <gmp/dp/object_factory.hpp>

#include <wuwe/agent/llm/openrouter_llm_client.h>
#include <wuwe/common/print.h>

WUWE_NAMESPACE_BEGIN

GMP_FACTORY_REGISTER(llm_client, llm_config, (OpenRouter, openrouter_llm_client))
using llm_client_factory = gmp::object_factory<llm_client, llm_config>;

WUWE_NAMESPACE_END

#endif // WUWE_WUWE_H
