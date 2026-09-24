#include <chrono>
#include <compare>
#include <string>
#include <utility>

#include <wuwe/wuwe.h>

namespace {

class package_factory_llm_client final : public wuwe::llm_client {
public:
  explicit package_factory_llm_client(const wuwe::llm_config& config) : model_(config.model) {
  }

  wuwe::llm_response complete(const wuwe::llm_request&) override {
    return { .content = model_ };
  }

private:
  std::string model_;
};

class package_model_list_http final : public wuwe::http_client {
public:
  wuwe::http_response send(const wuwe::http_request& request) override {
    if (request.method != "GET" || request.url != "https://api.xiaomimimo.com/v1/models") {
      return { .status_code = 400 };
    }
    return { .status_code = 200, .body = R"({"data":[{"id":"package-test-model"}]})" };
  }
};

} // namespace

int main() {
  static_assert(wuwe::framework_version_major == 1);
  static_assert(wuwe::framework_version_minor == 0);
  static_assert(wuwe::framework_version_patch == 0);
  if (wuwe::framework_version != "1.0.0") {
    return 1;
  }

  package_model_list_http discovery_http;
  wuwe::oauth_account_manager accounts(wuwe::make_memory_oauth_credential_store(), {});
  const wuwe::codex_oauth_options codex_options { .client_id = "package-test",
    .client_version = "1.0.0" };
  const auto codex_refresh = wuwe::make_codex_oauth_refresher(codex_options);
  wuwe::codex_oauth_client codex(accounts, codex_options);
  wuwe::codex_llm_client codex_generation(
    accounts, { "codex_oauth", "missing" }, { .model = "package-test", .client_version = "1.0.0" });
  if (!codex_generation.supports_streaming() || !codex_generation.capabilities().provider_state ||
      codex_generation.complete("hello").error_code != wuwe::oauth_error::account_not_found) {
    return 1;
  }
  if (codex_refresh->provider_id() != "codex_oauth" ||
      codex.list_models({ "codex_oauth", "missing" }).error !=
        wuwe::oauth_error::account_not_found) {
    return 1;
  }
  if (!accounts.accounts().empty() || accounts.access_token({ "missing", "account" }).error !=
                                        wuwe::oauth_error::account_not_found) {
    return 1;
  }
  const auto models = wuwe::list_llm_models("MiMo",
    { .api_key = "package-test-key", .load_api_key_from_environment = false },
    discovery_http);
  if (models.error_code || models.models.size() != 1 ||
      models.models[0].id != "package-test-model") {
    return 1;
  }
  for (const auto* id :
    { "Kimi", "MiniMax", "SiliconFlow", "Doubao", "Nvidia", "StepFun", "MiMo" }) {
    const auto client = wuwe::make_llm_client(id, { .load_api_key_from_environment = false });
    if (!client || !client->supports_streaming()) {
      return 1;
    }
  }

  constexpr auto factory_provider_id = "PackageFactoryExtension";
  wuwe::llm_client_factory factory;
  factory.unregister_type(factory_provider_id);
  wuwe::llm_client_factory::register_type<package_factory_llm_client> factory_registration(
    factory_provider_id);
  auto package_client = factory.create_unique(factory_provider_id,
    wuwe::llm_config {
      .model = "package-extension-model",
    });
  if (package_client->complete(wuwe::llm_request {}).content != "package-extension-model") {
    return 1;
  }

  wuwe::agent::skills::skill_registry skills;
  const auto skill_version = wuwe::agent::skills::semantic_version::parse("1.0.0");
  const auto linux_build = wuwe::agent::skills::semantic_version::parse("1.0.0+linux");
  const auto windows_build = wuwe::agent::skills::semantic_version::parse("1.0.0+windows");
  if (skill_version.major != 1 || !skills.snapshot().empty() || linux_build == windows_build ||
      wuwe::agent::skills::compare_precedence(linux_build, windows_build) !=
        std::strong_ordering::equal ||
      std::string(wuwe::agent::skills::to_string(
        wuwe::agent::skills::skill_error_code::registration_conflict)) != "registration_conflict") {
    return 1;
  }

  auto filesystem_backend = wuwe::agent::filesystem::make_local_filesystem_backend();
  auto process_backend = wuwe::agent::process::make_local_process_backend();
  if (!filesystem_backend || !process_backend || process_backend->info().name != "local_process") {
    return 1;
  }

  wuwe::agent::execution::controlled_process_backend_config config;
  config.validate_python_on_start = false;
  config.python_startup_timeout = std::chrono::milliseconds(3000);

  auto probe = wuwe::agent::execution::probe_python_interpreter({
    .interpreter = {},
    .timeout = std::chrono::milliseconds(100),
  });
  if (probe.status != wuwe::agent::execution::python_interpreter_status::empty_path ||
      wuwe::agent::execution::to_string(probe.status) != "empty_path" ||
      probe.metadata["error_code"] != "python_interpreter_empty_path") {
    return 1;
  }

  wuwe::agent::execution::execution_policy policy;
  policy.max_limits.max_code_bytes = 65536;
  auto backend = wuwe::agent::execution::make_controlled_process_backend();
  wuwe::agent::execution::execution_runtime runtime(std::move(backend), policy);
  wuwe::agent::execution::execution_tool_provider execution_tools(runtime);
  if (execution_tools.tools().empty()) {
    return 1;
  }

  auto registry = wuwe::agent::execution::make_default_execution_backend_registry();
  auto restricted_descriptor = wuwe::agent::execution::restricted_process_backend_descriptor();
  if (restricted_descriptor.available || restricted_descriptor.name != "restricted_process") {
    return 1;
  }
  wuwe::agent::execution::restricted_process_backend_config restricted_config;
  auto restricted_contract =
    wuwe::agent::execution::restricted_process_backend_configured_contract(restricted_config);
  auto restricted_availability =
    wuwe::agent::execution::evaluate_restricted_process_backend_availability(restricted_config);
  auto registered_restricted_availability =
    wuwe::agent::execution::evaluate_restricted_process_backend_availability(restricted_config,
      wuwe::agent::execution::restricted_process_backend_registration::registered_factory);
  if (!restricted_config.deny_network || !restricted_config.use_job_object ||
      restricted_config.inherit_parent_environment || !restricted_config.cleanup_runtime_staging ||
      wuwe::agent::execution::to_string(restricted_config.runtime_staging) !=
#ifdef _WIN32
        std::string("copy_minimal_python_runtime")) {
#else
        std::string("use_host_python")) {
#endif
    return 1;
  }
#if defined(_WIN32) || defined(__APPLE__)
  const auto expected_restricted_read_deny = wuwe::agent::sandbox::enforcement_level::enforced;
#else
  const auto expected_restricted_read_deny = wuwe::agent::sandbox::enforcement_level::not_enforced;
#endif
  if (restricted_contract.filesystem_read_deny != expected_restricted_read_deny) {
    return 1;
  }
  if (restricted_availability.available) {
    return 1;
  }
#if defined(_WIN32) || defined(__APPLE__)
  if (!registered_restricted_availability.available) {
    return 1;
  }
#else
  if (registered_restricted_availability.available) {
    return 1;
  }
#endif
  wuwe::agent::execution::execution_backend_requirements requirements;
  requirements.require_timeout = true;
  if (registry.select_backend_name(requirements) != "controlled_process") {
    return 1;
  }
  requirements.require_filesystem_read_deny = true;
  if (registry.create("restricted_process") != nullptr) {
    return 1;
  }
  if (registry.create_best(requirements) != nullptr) {
    return 1;
  }

  wuwe::agent::execution::execution_backend_registry_options registry_options;
  registry_options.enable_restricted_process_backend = true;
  auto explicit_registry =
    wuwe::agent::execution::make_execution_backend_registry(registry_options);
  auto explicit_restricted = explicit_registry.describe("restricted_process");
  auto explicit_restricted_backend = explicit_registry.create("restricted_process");
  if (!explicit_restricted.has_value()) {
    return 1;
  }
#if defined(_WIN32) || defined(__APPLE__)
  if (!explicit_restricted->available || explicit_restricted_backend == nullptr) {
    return 1;
  }
#else
  if (explicit_restricted->available || explicit_registry.create("restricted_process") != nullptr) {
    return 1;
  }
#endif
#ifdef __APPLE__
  wuwe::agent::execution::execution_request restricted_request;
  restricted_request.code =
    "try:\n open('/etc/passwd', 'rb').read(1)\n print('escaped')\nexcept OSError:\n "
    "print('isolated')";
  const auto restricted_result = explicit_restricted_backend->run(restricted_request, {});
  const auto bootstrap_environment = restricted_result.metadata.find("bootstrap_environment");
  if (restricted_result.exit_code != 0 || restricted_result.stdout_text != "isolated\n" ||
      bootstrap_environment == restricted_result.metadata.end() ||
      bootstrap_environment->second != "empty") {
    return 1;
  }
#endif

  wuwe::agent::mcp::mcp_server server;
  wuwe::agent::mcp::mcp_http_listener_options listener_options;
  listener_options.port = 0;
  wuwe::agent::mcp::mcp_http_listener listener(server, listener_options);
  server.add_root({ .uri = "file:///tmp/project", .name = "Project" });
  auto response = server.handle_message(R"({
    "jsonrpc":"2.0",
    "id":1,
    "method":"roots/list"
  })");
  return response.has_value() ? 0 : 1;
}
