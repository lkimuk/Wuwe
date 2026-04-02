#ifndef WUWE_AGENT_LLM_TYPES_H
#define WUWE_AGENT_LLM_TYPES_H

#include <wuwe/common/wuwe_fwd.h>
#include <wuwe/agent/llm/llm_error.h>

WUWE_AGENT_NAMESPACE_BEGIN

struct chat_message {
    std::string role;
    std::string content;
};

struct llm_request {
    std::string model;
    std::vector<chat_message> messages;
    double temperature {0.2};
    std::optional<std::string> response_format;
};

struct llm_usage {
    int prompt_tokens {0};
    int completion_tokens {0};
    int total_tokens {0};
};

struct llm_response {
    bool ok {false};
    std::string content;
    std::string error_message;
    llm_error_code error_code {llm_error_code::none};
    llm_usage usage;
};

WUWE_AGENT_NAMESPACE_END

#endif // WUWE_AGENT_LLM_TYPES_H
