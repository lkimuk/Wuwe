#include <iostream>

#include <windows.h>

#include <wuwe/agent/llm/openai_llm_client.h>

int main() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    wuwe::agent::llm_client_config config;
    config.base_url = "https://openrouter.ai/api";
    config.api_key = "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";
    config.default_model = "qwen/qwen3.6-plus-preview:free";
    config.timeout_ms = 30000;

    wuwe::agent::openai_llm_client client(config);

    wuwe::agent::llm_request request;
    request.messages.push_back({
        .role = "user",
        .content = "How about today?"
    });

    const auto response = client.complete(request);

    std::cout << "ok: " << std::boolalpha << response.ok << '\n';
    std::cout << "content: " << response.content << '\n';

    if (!response.error_message.empty()) {
        std::cout << "error_message: " << response.error_message << '\n';
    }

    return response.ok ? 0 : 1;
}
