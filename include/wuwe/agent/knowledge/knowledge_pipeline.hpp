#ifndef WUWE_AGENT_KNOWLEDGE_PIPELINE_HPP
#define WUWE_AGENT_KNOWLEDGE_PIPELINE_HPP

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <utility>

#include <wuwe/agent/knowledge/file_knowledge_index.hpp>
#include <wuwe/agent/knowledge/file_knowledge_store.hpp>
#include <wuwe/agent/knowledge/in_memory_knowledge_index.hpp>
#include <wuwe/agent/knowledge/in_memory_knowledge_store.hpp>
#include <wuwe/agent/knowledge/knowledge_context.hpp>
#include <wuwe/agent/knowledge/knowledge_retriever.hpp>
#include <wuwe/agent/knowledge/qdrant_knowledge_index.hpp>
#include <wuwe/agent/knowledge/sqlite_knowledge_index.hpp>

namespace wuwe::agent::knowledge {

class knowledge_pipeline {
public:
  class builder {
  public:
    builder& with_store(std::shared_ptr<knowledge_store> store) {
      store_ = std::move(store);
      return *this;
    }

    builder& with_index(std::shared_ptr<knowledge_index> index) {
      index_ = std::move(index);
      return *this;
    }

    builder& with_embedding_model(
      std::shared_ptr<::wuwe::agent::memory::embedding_model> embedding_model) {
      embedding_model_ = std::move(embedding_model);
      return *this;
    }

    builder& with_splitter(knowledge_splitter splitter) {
      splitter_ = std::move(splitter);
      return *this;
    }

    builder& with_indexing_policy(knowledge_indexing_policy policy) {
      indexing_policy_ = std::move(policy);
      return *this;
    }

    builder& with_context_policy(knowledge_policy policy) {
      context_policy_ = std::move(policy);
      return *this;
    }

    builder& local() {
      store_ = std::make_shared<in_memory_knowledge_store>();
      index_ = std::make_shared<in_memory_knowledge_index>();
      return *this;
    }

    builder& file_backed(
      std::filesystem::path store_path,
      std::filesystem::path index_path) {
      store_ = std::make_shared<file_knowledge_store>(std::move(store_path));
      index_ = std::make_shared<file_knowledge_index>(std::move(index_path));
      return *this;
    }

    builder& sqlite_index(
      std::filesystem::path store_path,
      std::filesystem::path index_path) {
      store_ = std::make_shared<file_knowledge_store>(std::move(store_path));
      index_ = std::make_shared<sqlite_knowledge_index>(std::move(index_path));
      return *this;
    }

    builder& qdrant_index(
      std::filesystem::path store_path,
      qdrant_knowledge_index_config config) {
      store_ = std::make_shared<file_knowledge_store>(std::move(store_path));
      index_ = std::make_shared<qdrant_knowledge_index>(std::move(config));
      return *this;
    }

    knowledge_pipeline build() {
      if (!store_) {
        store_ = std::make_shared<in_memory_knowledge_store>();
      }
      if (!index_) {
        index_ = std::make_shared<in_memory_knowledge_index>();
      }
      if (!embedding_model_) {
        throw std::invalid_argument("knowledge_pipeline requires an embedding_model");
      }

      auto retriever = std::make_shared<knowledge_retriever>(
        store_,
        index_,
        embedding_model_,
        std::move(splitter_),
        std::move(indexing_policy_));

      return knowledge_pipeline(
        std::move(store_),
        std::move(index_),
        std::move(embedding_model_),
        std::move(retriever),
        std::move(context_policy_));
    }

  private:
    std::shared_ptr<knowledge_store> store_;
    std::shared_ptr<knowledge_index> index_;
    std::shared_ptr<::wuwe::agent::memory::embedding_model> embedding_model_;
    knowledge_splitter splitter_;
    knowledge_indexing_policy indexing_policy_;
    knowledge_policy context_policy_;
  };

  static builder make() {
    return builder {};
  }

  knowledge_retriever& retriever() {
    return *retriever_;
  }

  const knowledge_retriever& retriever() const {
    return *retriever_;
  }

  knowledge_context context() const {
    return knowledge_context(retriever_, context_policy_);
  }

  std::shared_ptr<knowledge_retriever> retriever_ptr() const {
    return retriever_;
  }

private:
  knowledge_pipeline(
    std::shared_ptr<knowledge_store> store,
    std::shared_ptr<knowledge_index> index,
    std::shared_ptr<::wuwe::agent::memory::embedding_model> embedding_model,
    std::shared_ptr<knowledge_retriever> retriever,
    knowledge_policy context_policy)
      : store_(std::move(store)),
        index_(std::move(index)),
        embedding_model_(std::move(embedding_model)),
        retriever_(std::move(retriever)),
        context_policy_(std::move(context_policy)) {
  }

  std::shared_ptr<knowledge_store> store_;
  std::shared_ptr<knowledge_index> index_;
  std::shared_ptr<::wuwe::agent::memory::embedding_model> embedding_model_;
  std::shared_ptr<knowledge_retriever> retriever_;
  knowledge_policy context_policy_;
};

} // namespace wuwe::agent::knowledge

#endif // WUWE_AGENT_KNOWLEDGE_PIPELINE_HPP
