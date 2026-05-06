#ifndef WUWE_WUWE_H
#define WUWE_WUWE_H

#include <gmp/dp/object_factory.hpp>

#include <wuwe/agent/core/message.hpp>
#include <wuwe/agent/llm/llm_agent_runner.h>
#include <wuwe/agent/llm/openrouter_llm_client.h>
#include <wuwe/agent/knowledge/code_knowledge_loader.hpp>
#include <wuwe/agent/knowledge/directory_knowledge_loader.hpp>
#include <wuwe/agent/knowledge/file_knowledge_index.hpp>
#include <wuwe/agent/knowledge/file_knowledge_loader.hpp>
#include <wuwe/agent/knowledge/file_knowledge_store.hpp>
#include <wuwe/agent/knowledge/in_memory_knowledge_index.hpp>
#include <wuwe/agent/knowledge/in_memory_knowledge_store.hpp>
#include <wuwe/agent/knowledge/knowledge_benchmark.hpp>
#include <wuwe/agent/knowledge/knowledge_cache.hpp>
#include <wuwe/agent/knowledge/knowledge_context.hpp>
#include <wuwe/agent/knowledge/knowledge_document_loader.hpp>
#include <wuwe/agent/knowledge/knowledge_document_enricher.hpp>
#include <wuwe/agent/knowledge/knowledge_eval.hpp>
#include <wuwe/agent/knowledge/knowledge_grounding.hpp>
#include <wuwe/agent/knowledge/knowledge_metrics.hpp>
#include <wuwe/agent/knowledge/knowledge_migration.hpp>
#include <wuwe/agent/knowledge/knowledge_observability.hpp>
#include <wuwe/agent/knowledge/knowledge_parser_registry.hpp>
#include <wuwe/agent/knowledge/knowledge_path.hpp>
#include <wuwe/agent/knowledge/knowledge_pipeline.hpp>
#include <wuwe/agent/knowledge/knowledge_pipeline_config.hpp>
#include <wuwe/agent/knowledge/knowledge_query_rewriter.hpp>
#include <wuwe/agent/knowledge/knowledge_rag_service.hpp>
#include <wuwe/agent/knowledge/knowledge_reranker.hpp>
#include <wuwe/agent/knowledge/knowledge_result_processor.hpp>
#include <wuwe/agent/knowledge/knowledge_tools.hpp>
#include <wuwe/agent/knowledge/knowledge_text.hpp>
#include <wuwe/agent/knowledge/qdrant_knowledge_index.hpp>
#include <wuwe/agent/knowledge/remote_vector_knowledge_index.hpp>
#include <wuwe/agent/knowledge/sqlite_knowledge_index.hpp>
#include <wuwe/agent/knowledge/structured_knowledge_loader.hpp>
#include <wuwe/agent/knowledge/tika_knowledge_loader.hpp>
#include <wuwe/agent/memory/file_memory_store.hpp>
#include <wuwe/agent/memory/lexical_memory_ranker.hpp>
#include <wuwe/agent/memory/memory_context.hpp>
#include <wuwe/agent/memory/memory_tools.hpp>
#include <wuwe/agent/memory/sqlite_memory_store.hpp>
#include <wuwe/agent/orchestration/flow.hpp>
#include <wuwe/agent/tools/tool.hpp>
#include <wuwe/common/print.h>

WUWE_NAMESPACE_BEGIN

GMP_FACTORY_REGISTER(llm_client, llm_config, (OpenRouter, openrouter_llm_client))
using llm_client_factory = gmp::object_factory<llm_client, llm_config>;

WUWE_NAMESPACE_END

#endif // WUWE_WUWE_H
