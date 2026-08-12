#include <string>

#include <wuwe/agent/llm/llm_provider_factory.h>
#include <wuwe/agent/skills/skills.hpp>
#include <wuwe/version.hpp>

namespace {

class source_factory_llm_client final : public wuwe::llm_client {
public:
  explicit source_factory_llm_client(const wuwe::llm_config& config) : model_(config.model) {
  }

  wuwe::llm_response complete(const wuwe::llm_request&) override {
    return { .content = model_ };
  }

private:
  std::string model_;
};

} // namespace

int main() {
  static_assert(wuwe::framework_version_major == 1);
  static_assert(wuwe::framework_version_minor == 0);
  const wuwe::agent::skills::skill_registry registry;
  wuwe::llm_client_factory factory;
  constexpr auto provider_id = "SourceFactoryExtension";
  factory.unregister_type(provider_id);
  wuwe::llm_client_factory::register_type<source_factory_llm_client> registration(provider_id);
  auto client = factory.create_unique(provider_id,
    wuwe::llm_config {
      .model = "source-extension-model",
    });
  return wuwe::framework_version == "1.0.0" && registry.snapshot().empty() &&
             client->complete(wuwe::llm_request {}).content == "source-extension-model"
           ? 0
           : 1;
}
