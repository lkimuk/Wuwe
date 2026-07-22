#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "console_utf8.hpp"

#include <wuwe/wuwe.h>

namespace {

std::string env_or(const char* name, std::string fallback = {}) {
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
  static constexpr std::string_view description = "Get the current weather for a city.";

  std::string city;

  std::string invoke() const {
    if (city == "Tokyo") {
      return "Tokyo is 22C and cloudy.";
    }
    return city + " weather data is unavailable in this demo.";
  }
};

void run_offline_plan_reasoning() {
  namespace planning = wuwe::agent::planning;
  namespace reasoning = wuwe::agent::reasoning;

  auto planner = std::make_shared<planning::static_planner>(std::vector<planning::plan_step> {
    {
      .id = "inspect",
      .title = "Inspect input",
    },
    {
      .id = "summarize",
      .title = "Summarize result",
      .depends_on = { "inspect" },
    },
  });

  auto executor = std::make_shared<planning::function_plan_executor>(
    [](const planning::plan_step& step, const planning::plan_execution_context&) {
      if (step.id == "inspect") {
        return planning::plan_step_result::completed("input inspected");
      }
      return planning::plan_step_result::completed("plan completed after inspection");
    });

  reasoning::reasoning_runner runner({
    .planner = planner,
    .executor = executor,
    .observer = [](const reasoning::reasoning_event& event) {
      if (event.type == reasoning::reasoning_event_type::plan_step_started) {
        wuwe::println("[plan step] {}", event.step_id);
      }
    },
  });

  const auto result = runner.run({
    .input = "Use a tiny static plan.",
    .policy = {
      .mode = reasoning::reasoning_mode::plan_execute,
    },
  });

  wuwe::println("offline plan result: {}\n", result.content);
  wuwe::println("offline usage: plan_steps={}, trace_events={}\n",
    result.usage.plan_steps,
    result.trace.size());
}

void run_live_react_reasoning() {
  namespace reasoning = wuwe::agent::reasoning;

  wuwe::llm_config config {
    .base_url = env_or("OPENROUTER_BASE_URL", "https://openrouter.ai/api"),
    .api_key = wuwe::llm_client_config::load_openrouter_api_key_from_env(),
    .model = env_or("OPENROUTER_CHAT_MODEL", "openai/gpt-oss-120b:free"),
    .timeout = 60000,
  };

  if (config.api_key.empty() && config.base_url.find("openrouter.ai") != std::string::npos) {
    wuwe::println("Set OPENROUTER_API_KEY to run the live ReAct reasoning example.");
    return;
  }

  wuwe::llm_client_factory factory;
  auto client = factory.create_shared("OpenRouter", config);
  auto provider = std::make_shared<wuwe::tool_provider<get_weather>>();

  auto runner = reasoning::reasoning_runner::with_tools(*client, provider, {
    .observer = [](const reasoning::reasoning_event& event) {
      if (event.type == reasoning::reasoning_event_type::content_delta) {
        wuwe::print("{}", event.delta);
        std::cout << std::flush;
      }
      else if (event.type == reasoning::reasoning_event_type::tool_started) {
        wuwe::println("\n[tool] {}", event.message);
      }
    },
  });

  wuwe::print("live react result: ");
  const auto result = runner.run({
    .input = "What's the weather in Tokyo? Use the tool if it helps. Answer in one sentence.",
    .model = config.model,
    .policy = {
      .mode = reasoning::reasoning_mode::react,
      .budget = {
        .max_tool_rounds = 2,
      },
      .enable_streaming = true,
    },
  });

  if (!result) {
    wuwe::println("\nerror: {}", result.error.empty() ? result.error_code.message() : result.error);
    return;
  }
  wuwe::println("\nusage: model_calls={}, tool_calls={}, trace_events={}\n",
    result.usage.model_calls,
    result.usage.tool_calls,
    result.trace.size());
}

} // namespace

int main() {
  wuwe_example::configure_utf8_console();

  run_offline_plan_reasoning();
  run_live_react_reasoning();
  return 0;
}
