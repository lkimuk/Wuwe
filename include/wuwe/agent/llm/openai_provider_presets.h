#ifndef WUWE_AGENT_LLM_OPENAI_PROVIDER_PRESETS_H
#define WUWE_AGENT_LLM_OPENAI_PROVIDER_PRESETS_H

#include <memory>

#include <wuwe/agent/llm/openai_compatible_llm_client.h>

WUWE_NAMESPACE_BEGIN

class openai_llm_client final : public openai_compatible_llm_client {
public:
  explicit openai_llm_client(llm_client_config config);
  openai_llm_client(llm_client_config config, std::shared_ptr<http_client> http);

private:
  static llm_client_config normalize_config(llm_client_config config);
};

class deepseek_llm_client final : public openai_compatible_llm_client {
public:
  explicit deepseek_llm_client(llm_client_config config);
  deepseek_llm_client(llm_client_config config, std::shared_ptr<http_client> http);

private:
  static llm_client_config normalize_config(llm_client_config config);
};

class dashscope_llm_client : public openai_compatible_llm_client {
public:
  explicit dashscope_llm_client(llm_client_config config);
  dashscope_llm_client(llm_client_config config, std::shared_ptr<http_client> http);

private:
  static llm_client_config normalize_config(llm_client_config config);
};

class qwen_llm_client final : public openai_compatible_llm_client {
public:
  explicit qwen_llm_client(llm_client_config config);
  qwen_llm_client(llm_client_config config, std::shared_ptr<http_client> http);

private:
  static llm_client_config normalize_config(llm_client_config config);
};

class zhipu_llm_client final : public openai_compatible_llm_client {
public:
  explicit zhipu_llm_client(llm_client_config config);
  zhipu_llm_client(llm_client_config config, std::shared_ptr<http_client> http);

private:
  static llm_client_config normalize_config(llm_client_config config);
};

class kimi_llm_client final : public openai_compatible_llm_client {
public:
  explicit kimi_llm_client(llm_client_config config);
  kimi_llm_client(llm_client_config config, std::shared_ptr<http_client> http);
};

class minimax_llm_client final : public openai_compatible_llm_client {
public:
  explicit minimax_llm_client(llm_client_config config);
  minimax_llm_client(llm_client_config config, std::shared_ptr<http_client> http);
};

class siliconflow_llm_client final : public openai_compatible_llm_client {
public:
  explicit siliconflow_llm_client(llm_client_config config);
  siliconflow_llm_client(llm_client_config config, std::shared_ptr<http_client> http);
};

class doubao_llm_client final : public openai_compatible_llm_client {
public:
  explicit doubao_llm_client(llm_client_config config);
  doubao_llm_client(llm_client_config config, std::shared_ptr<http_client> http);
};

class nvidia_llm_client final : public openai_compatible_llm_client {
public:
  explicit nvidia_llm_client(llm_client_config config);
  nvidia_llm_client(llm_client_config config, std::shared_ptr<http_client> http);
};

class stepfun_llm_client final : public openai_compatible_llm_client {
public:
  explicit stepfun_llm_client(llm_client_config config);
  stepfun_llm_client(llm_client_config config, std::shared_ptr<http_client> http);
};

class mimo_llm_client final : public openai_compatible_llm_client {
public:
  explicit mimo_llm_client(llm_client_config config);
  mimo_llm_client(llm_client_config config, std::shared_ptr<http_client> http);
};

WUWE_NAMESPACE_END

#endif // WUWE_AGENT_LLM_OPENAI_PROVIDER_PRESETS_H
