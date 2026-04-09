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

  auto chain = wuwe::prompt_chain_builder {}
                 .tool("lookup_weather",
                   {
                     .invoke = [](wuwe::chain_state&) {
                       return wuwe::llm_tool_result {
                         .content = R"({"city":"Tokyo","summary":"cloudy","high":72,"unit":"fahrenheit"})"
                       };
                     },
                     .result_key = "weather",
                     .next_step = "draft"
                   })
                 .llm("draft",
                   {
                     .system_prompt = "Write a short, friendly answer using the provided weather JSON.",
                     .prompt = [](const wuwe::chain_state& state) {
                       return "User input: " + state.input +
                              "\nWeather JSON: " + state.get_json("weather").dump();
                     },
                     .state_key = "final_output"
                   })
                 .build("lookup_weather");

  const auto result = chain.run(*client, wuwe::chain_state { "Tell me the weather in Tokyo." });
  if (result) {
    wuwe::println("chain output: {}", result.output);
  }
  else {
    if (result.error_code == wuwe::net_errc::rate_limited) {
      wuwe::println(
        "chain error: rate limited by provider (HTTP 429). Retry later or switch to a non-free model.");
    }
    else {
      wuwe::println("chain error: {}", result.error_message);
    }
  }

  return result.error_code.value();
}
