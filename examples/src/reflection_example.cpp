#include <wuwe/agent/llm/openrouter_llm_client.h>
#include <wuwe/agent/reflection/reflection.hpp>
#include <wuwe/common/print.h>

#include <cstdlib>
#include <exception>
#include <memory>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

std::string env_value(const char* name, std::string fallback = {}) {
#ifdef _WIN32
  char* raw = nullptr;
  size_t length = 0;
  if (_dupenv_s(&raw, &length, name) != 0 || raw == nullptr) {
    return fallback;
  }
  std::string value(raw);
  free(raw);
#else
  const char* raw = std::getenv(name);
  if (raw == nullptr) {
    return fallback;
  }
  std::string value(raw);
#endif
  if (value.empty()) {
    return fallback;
  }
  return value;
}

} // namespace

int main() {
#ifdef _WIN32
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
#endif

  namespace reflection = wuwe::agent::reflection;

  const auto api_key = wuwe::llm_client_config::load_api_key_from_env();
  if (api_key.empty()) {
    wuwe::println("missing OPENROUTER_API_KEY");
    wuwe::println("PowerShell:");
    wuwe::println("  $env:OPENROUTER_API_KEY = \"your_api_key\"");
    wuwe::println("  $env:OPENROUTER_CHAT_MODEL = \"openai/gpt-oss-120b:free\"");
    wuwe::println("  .\\build-mcp\\examples\\Debug\\reflection_example.exe");
    return 1;
  }

  const std::string model = env_value("OPENROUTER_CHAT_MODEL", "openai/gpt-oss-120b:free");
  wuwe::openrouter_llm_client client({
    .base_url = "https://openrouter.ai/api",
    .api_key = api_key,
    .model = model,
    .timeout = 30000,
  });

  auto reflector = std::make_shared<reflection::llm_reflector>(
    client,
    reflection::llm_reflector_options {
      .model = model,
      .temperature = 0.0,
    });

  reflection::reflection_runner runner({
    .reflector = reflector,
  });

  try {
    const auto run = runner.run({
      .task = "Evaluate whether the candidate answer accurately explains Planning vs Reflection.",
      .original_input = "What is the difference between Planning and Reflection in an agent framework?",
      .candidate_output =
        "Planning and Reflection are basically the same. Both split the task into steps before "
        "execution, so a separate Reflection module is unnecessary.",
      .context =
        "Planning is forward-looking: it decomposes a goal into future actions. Reflection is "
        "backward-looking: it evaluates an existing output or execution result and suggests "
        "revision, retry, replanning, blocking, or escalation.",
      .subject_type = "final_answer",
      .rubric = {
        .criteria = {
          {
            .name = "conceptual_accuracy",
            .description =
              "The answer must clearly distinguish Planning from Reflection by purpose, timing, "
              "input, output, and lifecycle role.",
            .weight = 0.7,
            .pass_threshold = 0.8,
          },
          {
            .name = "actionability",
            .description =
              "The evaluator should recommend a concrete next action and revision guidance.",
            .weight = 0.3,
            .pass_threshold = 0.75,
          },
        },
        .pass_threshold = 0.8,
        .require_evidence = true,
        .allow_revision = true,
      },
    });

    wuwe::println("model: {}", model);
    wuwe::println("passed: {}", run.result.passed ? "true" : "false");
    wuwe::println("score: {:.2f}", run.result.score);
    wuwe::println("action: {}", reflection::to_string(run.result.recommended_action));

    for (const auto& issue : run.result.issues) {
      wuwe::println("issue: [{}] {} - {}",
        reflection::to_string(issue.severity),
        issue.code,
        issue.message);
      if (!issue.evidence.empty()) {
        wuwe::println("evidence: {}", issue.evidence);
      }
      if (!issue.suggestion.empty()) {
        wuwe::println("suggestion: {}", issue.suggestion);
      }
    }

    if (!run.result.revised_output.empty()) {
      wuwe::println("revised_output: {}", run.result.revised_output);
    }
  }
  catch (const std::exception& error) {
    wuwe::println("reflection failed: {}", error.what());
    return 2;
  }

  return 0;
}
