#include <iostream>

#include <windows.h>

#include <wuwe/net/net_errc.h>
#include <wuwe/wuwe.h>

int main() {
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);

  wuwe::llm_config config {
    .base_url = "https://openrouter.ai/api",
    .model = "openai/gpt-oss-120b:free",
    .timeout = 30000,
  };

  wuwe::llm_client_factory factory;
  auto client = factory.create_shared("OpenRouter", config);

  wuwe::llm_tool_registry registry;
  registry.register_tool({ .name = "get_happy_fact",
                           .description = "Get one uplifting fact to cheer up the user.",
                           .parameters_json_schema = R"({
        "type":"object",
        "properties":{},
        "additionalProperties":false
      })" },
    [](const std::string&) {
      return wuwe::llm_tool_result {
        .content = R"({"fact":"A 20-minute walk can noticeably improve mood for many people."})"
      };
    });

  wuwe::llm_agent_runner runner(*client, &registry);
  const auto response =
    runner.complete("Tell me some happy things. Use tools when it helps provide a better answer.");
  if (response) {
    wuwe::println("content: {}", response.content);
  }
  else {
    if (response.error_code == wuwe::net_errc::rate_limited) {
      wuwe::println(
        "error: rate limited by provider (HTTP 429). Retry later or switch to a non-free model.");
    }
    else {
      wuwe::println("error: {}", response.error_code.message());
    }
  }

  return response.error_code.value();
}
