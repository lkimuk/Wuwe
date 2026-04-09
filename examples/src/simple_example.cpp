#include <windows.h>
#include <wuwe/wuwe.h>

struct get_weather {
  static constexpr std::string_view description = "Get the current weather for a given city.";

  std::string city;

  std::string invoke() const {
    if (city == "Tokyo") {
      return "Tokyo is 22C and cloudy.";
    }
    if (city == "Shanghai") {
      return "Shanghai is 24C with light rain.";
    }
    return city + " weather data is unavailable in this demo.";
  }
};

int main() {
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
  wuwe::llm_config config {
    .base_url = "https://openrouter.ai/api",
    .model = "openai/gpt-oss-120b:free",
  };

  wuwe::llm_client_factory factory;
  auto client = factory.create_shared("OpenRouter", config);
  auto runner = client->bind_tools<get_weather>();

  const auto response = runner.complete("What's the weather in Shanghai?");

  wuwe::println("{}", response.content);
}
