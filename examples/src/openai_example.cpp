#include <iostream>

#include <windows.h>

#include <wuwe/agent/llm/openai_llm_client.h>
#include <wuwe/net/net_errc.h>

int main() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    wuwe::agent::llm_client_config config;
    config.base_url = "https://openrouter.ai/api";
    config.api_key = "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";
    config.default_model = "qwen/qwen3.6-plus:free";
    config.timeout_ms = 30000;

    wuwe::agent::openai_llm_client client(config);

    wuwe::agent::llm_request request;
    request.messages.push_back({
        .role = "user",
        .content = "How about today?"
    });

    const auto response = client.complete(request);
    if (!response.error_code) {
        std::cout << "content: " << response.content << '\n';
    } else {
        std::cout << "error_code: " << response.error_code.message() << '\n';
        std::cout << "is timeout: " << std::boolalpha
                  << (response.error_code == wuwe::net_errc::timeout) << '\n';
        std::cout << "is name_resolution_failed: " << std::boolalpha
                  << (response.error_code == wuwe::net_errc::name_resolution_failed) << '\n';
        std::cout << "is connection_failed: " << std::boolalpha
                  << (response.error_code == wuwe::net_errc::connection_failed) << '\n';
        std::cout << "is tls_failed: " << std::boolalpha
                  << (response.error_code == wuwe::net_errc::tls_failed) << '\n';
        std::cout << "is transport_failed: " << std::boolalpha
                  << (response.error_code == wuwe::net_errc::transport_failed) << '\n';
        std::cout << "is server_error: " << std::boolalpha
                  << (response.error_code == wuwe::net_errc::server_error) << '\n';
        std::cout << "is service_unavailable: " << std::boolalpha
                  << (response.error_code == wuwe::net_errc::service_unavailable) << '\n';
    }

    return response.error_code.value();
}
