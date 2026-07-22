#include <cstdlib>
#include <iostream>
#include <string>

#include "console_utf8.hpp"

#include <wuwe/net/net_errc.h>
#include <wuwe/wuwe.h>

namespace {

std::string env_or(const char* name, std::string fallback) {
#if defined(_MSC_VER)
  char* value {};
  std::size_t size {};
  if (_dupenv_s(&value, &size, name) != 0 || !value) {
    return fallback;
  }
  std::string result(value);
  std::free(value);
  return result.empty() ? fallback : result;
#else
  const char* value = std::getenv(name);
  return value && *value != '\0' ? std::string(value) : fallback;
#endif
}

struct get_weather {
  static constexpr std::string_view description = "Get the current weather in a given location.";

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

void print_error(const wuwe::llm_response& response) {
  if (response.error_code == wuwe::net_errc::rate_limited) {
    wuwe::println(
      "\nerror: rate limited by provider (HTTP 429). Retry later or choose another model.");
    return;
  }
  wuwe::println("\nerror: {}", response.error_code.message());
  if (!response.content.empty()) {
    wuwe::println("provider response: {}", response.content);
  }
}

} // namespace

int main() {
  wuwe_example::configure_utf8_console();

  wuwe::llm_config config {
    .base_url = env_or("OPENROUTER_BASE_URL", "https://openrouter.ai/api"),
    .api_key = wuwe::llm_client_config::load_openrouter_api_key_from_env(),
    .model = env_or("OPENROUTER_CHAT_MODEL", "openai/gpt-oss-120b:free"),
    .timeout = 60000,
    .stream_timeouts = {
      .total_ms = 60000,
      .connect_ms = 10000,
      .first_event_ms = 20000,
      .idle_ms = 15000,
    },
  };

  if (config.api_key.empty() && config.base_url.find("openrouter.ai") != std::string::npos) {
    wuwe::println("Set OPENROUTER_API_KEY before running this example.");
    return 1;
  }

  wuwe::llm_client_factory factory;
  auto client = factory.create_shared("OpenRouter", config);

  wuwe::println("Direct complete_stream() example");
  wuwe::println("model={}", config.model);
  wuwe::print("assistant: ");

  wuwe::llm_request direct_request;
  direct_request.model = config.model;
  direct_request.messages.push_back({
    .role = "user",
    .content = "Explain streaming LLM output in two concise sentences.",
  });

  wuwe::llm_stream_callbacks stream_callbacks;
  stream_callbacks.on_event = [](const wuwe::llm_stream_event& event) {
    if (event.type == wuwe::llm_stream_event_type::content_delta) {
      wuwe::print("{}", event.content_delta);
      std::cout << std::flush;
    }
    else if (event.type == wuwe::llm_stream_event_type::tool_call_delta &&
             event.tool_call_delta.has_value()) {
      const auto& delta = *event.tool_call_delta;
      if (!delta.name_delta.empty()) {
        wuwe::print("\n[tool name delta] {}", delta.name_delta);
      }
      if (!delta.arguments_delta.empty()) {
        wuwe::print("\n[tool args delta] {}", delta.arguments_delta);
      }
      std::cout << std::flush;
    }
  };

  const auto direct_response = client->complete_stream(direct_request, stream_callbacks);
  if (!direct_response) {
    print_error(direct_response);
    return direct_response.error_code.value();
  }

  wuwe::println("\n\nRunner on_delta example with tools");
  auto runner = client->bind_tools<get_weather>();
  wuwe::llm_agent_run_options options;
  options.callbacks.on_delta = [](std::string_view delta) {
    wuwe::print("{}", delta);
    std::cout << std::flush;
  };
  options.callbacks.on_event = [](const wuwe::llm_agent_event& event) {
    if (event.type == wuwe::llm_agent_event_type::tool_call_building) {
      wuwe::print("\n[tool call building]");
      std::cout << std::flush;
    }
    else if (event.type == wuwe::llm_agent_event_type::tool_call_ready && event.tool_call) {
      wuwe::println("\n[tool call ready] {}", event.tool_call->name);
    }
  };
  options.callbacks.on_tool_start = [](const wuwe::llm_tool_call& call) {
    wuwe::println("\n[tool start] {} {}", call.name, call.arguments_json);
  };
  options.callbacks.on_tool_result =
    [](const wuwe::llm_tool_call& call, const wuwe::llm_tool_result& result) {
      wuwe::println("[tool result] {} -> {}", call.name, result.content);
      wuwe::print("assistant: ");
    };

  wuwe::print("assistant: ");
  const auto runner_response = runner.complete(
    "What's the weather in Tokyo? Use the tool if it helps, then answer in one sentence.",
    std::move(options));

  if (!runner_response) {
    print_error(runner_response);
    return runner_response.error_code.value();
  }

  wuwe::println("\n\nusage: prompt_tokens={} completion_tokens={} total_tokens={}",
    runner_response.usage.prompt_tokens,
    runner_response.usage.completion_tokens,
    runner_response.usage.total_tokens);

  return 0;
}
