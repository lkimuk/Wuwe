#include <algorithm>
#include <iostream>
#include <string>

#include <wuwe/agent/net/default_http_client.h>

int main() {
    wuwe::agent::default_http_client client;

    wuwe::agent::http_request request;
    request.method = "GET";
    request.url = "https://postman-echo.com/get?ping=1";
    request.headers = {
        {"Accept", "application/json"},
        {"User-Agent", "wuwe-example/0.1"}
    };

    const auto response = client.send(request);

    std::cout << "ok: " << std::boolalpha << response.ok << '\n';
    std::cout << "status_code: " << response.status_code << '\n';

    if (!response.error_message.empty()) {
        std::cout << "error_message: " << response.error_message << '\n';
    }

    const std::size_t preview_size = std::min<std::size_t>(response.body.size(), 300);
    std::cout << "body_preview:\n";
    std::cout << response.body.substr(0, preview_size) << '\n';

    return response.ok ? 0 : 1;
}
