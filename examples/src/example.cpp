#include <iostream>

#include <windows.h>

#include <wuwe/net/net_errc.h>
#include <wuwe/wuwe.h>

struct get_weather {
  static constexpr std::string_view description = "Get the current weather in a given location.";

  std::string city;

  std::string invoke() const {
    if (city == "New York") {
      return "The weather in New York is sunny with a high of 25°C.";
    }
    else if (city == "London") {
      return "The weather in London is rainy with a high of 18°C.";
    }
    else if (city == "Tokyo") {
      return "The weather in Tokyo is cloudy with a high of 22°C.";
    }
    else {
      return "Sorry, I don't have weather information for that city.";
    }
  }
};

template <typename T> void print() {
  wuwe::println("name: {}\ndescription: {}", gmp::type_name<T>().to_string_view(), T::description);
  wuwe::println("Members:");
  auto member_names = gmp::member_names<T>();
  auto member_type_names = gmp::member_type_names<T>();
  for (unsigned i = 0; i < member_names.size(); ++i) {
    wuwe::println("{} {}", member_type_names[i], member_names[i]);
  }
}

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

  auto runner = client->build_tools<get_weather>();
  const auto response = runner.complete(
    "What's the weather in Tokyo? Use tools when it helps provide a better answer.");
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

  // print<get_weather>();

  return response.error_code.value();
}
