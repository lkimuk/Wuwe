#include <windows.h>
#include <wuwe/wuwe.h>

int main() {
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
  wuwe::llm_config config {
    .base_url = "https://openrouter.ai/api",
    .model = "openai/gpt-oss-120b:free",
  };

  wuwe::llm_client_factory factory;
  auto client = factory.create_shared("OpenRouter", config);

  auto extract = [](const std::string& prompt) {
    return "Extract the technical specifications from the following text:\n\n" + prompt;
  };
  auto transform = [](const wuwe::llm_response& response) {
    return "Transform the following specifications into a JSON object with 'cpu', 'memory', and "
           "'storage' as keys:\n\n" +
           response.content;
  };

  auto chain = client | extract | transform;
  auto response = chain.invoke("The new laptop model features a 3.5 GHz octa-core processor, "
                               "16GB of RAM, and a 1TB NVMe SSD.");
  wuwe::print("{}", response.content);
}
