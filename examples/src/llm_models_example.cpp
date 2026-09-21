#include <iostream>
#include <utility>

#include <wuwe/agent/llm/llm_model_discovery.h>

int main(int argc, char** argv) {
  if (argc < 2 || argc > 3) {
    std::cerr << "Usage: llm_models_example <provider-id> [base-url]\n"
                 "Credentials are read from the provider's environment variables.\n";
    return 2;
  }
  wuwe::llm_client_config config;
  if (argc == 3) {
    config.base_url = argv[2];
  }
  const auto result = wuwe::list_llm_models(argv[1], std::move(config));
  if (result.error_code) {
    std::cerr << result.error_code.message() << " (HTTP " << result.http_status << ")\n";
    return 1;
  }
  for (const auto& model : result.models) {
    std::cout << model.id;
    if (model.display_name) {
      std::cout << '\t' << *model.display_name;
    }
    std::cout << '\n';
  }
  return 0;
}
