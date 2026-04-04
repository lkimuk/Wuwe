#include <iostream>

#include <windows.h>

#include <wuwe/net/net_errc.h>
#include <wuwe/wuwe.h>

int main() {
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);

  wuwe::llm_config config {
    .base_url = "https://openrouter.ai/api",
    .model = "qwen/qwen3.6-plus:free",
    .timeout = 30000,
  };

  wuwe::llm_client_factory factory;
  auto client = factory.create_shared("OpenRouter", config);

  const auto response = client->complete("Tell me some happy things?");
  if (response) {
    wuwe::println("content: {}", response.content);
  }
  else {
    wuwe::println("error: {}", response.error_code.message());
  }

  return response.error_code.value();
}
