#include <iostream>

#include <windows.h>

#include <wuwe/net/net_errc.h>
#include <wuwe/wuwe.h>

enum class temperature_unit {
  celsius,
  fahrenheit
};

struct weather_report {
  std::string city;
  std::string summary;
  int high;
  temperature_unit unit;
  std::optional<std::string> advisory;
};

struct get_weather {
  static constexpr std::string_view description = "Get the current weather in a given location.";

  std::string city;
  temperature_unit unit { temperature_unit::celsius };

  weather_report invoke() const {
    if (city == "New York") {
      return {
        .city = city,
        .summary = "sunny",
        .high = unit == temperature_unit::celsius ? 25 : 77,
        .unit = unit,
        .advisory = "A light jacket is enough for the evening."
      };
    }
    else if (city == "London") {
      return {
        .city = city,
        .summary = "rainy",
        .high = unit == temperature_unit::celsius ? 18 : 64,
        .unit = unit,
        .advisory = "Bring an umbrella."
      };
    }
    else if (city == "Tokyo") {
      return {
        .city = city,
        .summary = "cloudy",
        .high = unit == temperature_unit::celsius ? 22 : 72,
        .unit = unit,
        .advisory = std::nullopt
      };
    }
    else {
      return {
        .city = city,
        .summary = "unknown",
        .high = 0,
        .unit = unit,
        .advisory = "Sorry, I don't have weather information for that city."
      };
    }
  }
};

template <>
struct wuwe::llm_tool_field_traits<get_weather, 0> {
  static constexpr std::string_view description = "Target city, for example New York or Tokyo.";
};

template <>
struct wuwe::llm_tool_field_traits<get_weather, 1> {
  static constexpr std::string_view description = "Preferred unit for the reported temperature.";

  static constexpr temperature_unit default_value() {
    return temperature_unit::celsius;
  }
};

struct get_happy_fact {
  static constexpr std::string_view description = "Get one uplifting fact to cheer up the user.";

  struct result {
    std::string fact;
    int mood_boost;
  };

  result invoke() const {
    return {
      .fact = "A 20-minute walk can noticeably improve mood for many people.",
      .mood_boost = 7
    };
  }
};

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

  auto runner = client->build_tools<get_weather, get_happy_fact>();
  const auto response = runner.complete(
    "What's the weather in Tokyo in fahrenheit? Also share one happy fact. Use tools when it helps.");
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
