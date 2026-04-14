#include <windows.h>

#include <wuwe/agent/orchestration/flow_primitives.hpp>
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

  auto to_extraction_prompt = [](const std::string& input) {
    return "Extract the technical specifications from the following product description as "
           "concise bullet points.\n\n" +
           input;
  };

  auto to_json_prompt = wuwe::if_else(
    [](const wuwe::llm_response& response) {
      return response.content.find("Processor") != std::string::npos ||
             response.content.find("CPU") != std::string::npos;
    },
    [](const wuwe::llm_response& response) {
      return "Transform the following specifications into compact JSON with keys 'cpu', "
             "'memory', and 'storage'. Use null when a value is missing.\n\n" +
             response.content;
    },
    [](const wuwe::llm_response& response) {
      return "Rewrite the following result as compact JSON with a single key named 'summary'.\n\n" +
             response.content;
    });

  auto pipeline =
    client | to_extraction_prompt | wuwe::tee([](const wuwe::llm_response& response) {
      wuwe::println("first pass:\n{}", response.content);
    }) |
    wuwe::filter(
      [](const wuwe::llm_response& response) { return response && !response.content.empty(); },
      "first llm pass returned no content") |
    to_json_prompt |
    wuwe::retry_if(
      [](const wuwe::llm_response& response) {
        return response.error_code == wuwe::net_errc::rate_limited;
      },
      1) |
    wuwe::recover([](const std::exception& e) {
      return wuwe::llm_response { .content = std::string("flow failed: ") + e.what() };
    });

  const auto response = pipeline.invoke("The new laptop model features a 3.5 GHz octa-core "
                                        "processor, 16GB of RAM, and a 1TB NVMe SSD.");

  if (response) {
    wuwe::println("final output:\n{}", response.content);
  }
  else {
    wuwe::println("error: {}", response.error_code.message());
  }

  return response.error_code.value();
}
