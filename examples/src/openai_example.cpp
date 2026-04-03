#include <iostream>

#include <windows.h>

#include <wuwe/agent/llm/openai_llm_client.h>
#include <wuwe/net/net_errc.h>

int main()
{
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);

  wuwe::agent::llm_client_config config;
  config.base_url = "https://openrouter.ai/api";
  config.api_key = "your-api-key";
  config.default_model = "qwen/qwen3.6-plus:free";
  config.timeout_ms = 30000;

  wuwe::agent::openai_llm_client client(config);

  wuwe::agent::llm_request request;
  request.messages.push_back({.role = "user", .content = "Tell me some happy things?"});

  const auto response = client.complete(request);
  if (!response.error_code)
  {
    std::cout << "content: " << response.content << '\n';
  }
  else
  {
    std::cout << "error_code: " << response.error_code.message() << '\n';
  }

  return response.error_code.value();
}
