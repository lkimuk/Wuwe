#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <wuwe/agent/approval/approval_service.hpp>
#include <wuwe/agent/llm/llm_agent_runner.h>
#include <wuwe/agent/runtime/run_runtime.hpp>
#include <wuwe/runtime_host.hpp>

#include "idempotency_repository.hpp"

namespace {

using namespace std::chrono_literals;

void expect(bool condition, const std::string& message) {
  if (!condition)
    throw std::runtime_error(message);
}

template<typename Exception, typename Callable>
void expect_throws(Callable&& callable, const std::string& message) {
  try {
    std::forward<Callable>(callable)();
  }
  catch (const Exception&) {
    return;
  }
  throw std::runtime_error(message);
}

template<typename Callable>
void expect_plugin_error(wuwe::runtime_host::plugin_package_error_code code, Callable&& callable,
  const std::string& message) {
  try {
    std::forward<Callable>(callable)();
  }
  catch (const wuwe::runtime_host::plugin_package_error& exception) {
    expect(exception.code() == code, message + ": unexpected error code");
    return;
  }
  throw std::runtime_error(message);
}

template<typename Callable>
void expect_plugin_process_error(wuwe::runtime_host::plugin_process_error_code code,
  Callable&& callable, const std::string& message) {
  try {
    std::forward<Callable>(callable)();
  }
  catch (const wuwe::runtime_host::plugin_process_error& exception) {
    expect(exception.code() == code, message + ": unexpected error code");
    return;
  }
  throw std::runtime_error(message);
}

template<typename Callable>
void expect_plugin_runtime_error(wuwe::runtime_host::plugin_runtime_error_code code,
  Callable&& callable, const std::string& message) {
  try {
    std::forward<Callable>(callable)();
  }
  catch (const wuwe::runtime_host::plugin_runtime_error& exception) {
    expect(exception.code() == code, message + ": unexpected error code");
    return;
  }
  throw std::runtime_error(message);
}

nlohmann::json valid_config_document() {
  return {
    { "schemaVersion", 1 },
    { "runtime",
      {
        { "maxConcurrentRuns", 2 },
        { "defaultRunTimeoutMs", 5000 },
        { "submissionTimeoutMs", 1000 },
        { "shutdownTimeoutMs", 2000 },
      } },
    { "llm",
      {
        { "provider", "OpenAI" },
        { "model", "test-model" },
        { "apiKeyEnv", "WUWE_RUNTIME_TEST_KEY" },
        { "timeoutMs", 1000 },
        { "maxRetries", 0 },
      } },
    { "store", { { "kind", "memory" } } },
    { "serve", { { "transport", "stdio" } } },
  };
}

class temporary_directory final {
public:
  explicit temporary_directory(std::string_view purpose)
      : path_(std::filesystem::temp_directory_path() /
              ("wuwe-runtime-" + std::string(purpose) + "-" +
                wuwe::agent::runtime::agent_run_runtime::make_identifier("test"))) {
    std::filesystem::create_directories(path_);
  }

  ~temporary_directory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  temporary_directory(const temporary_directory&) = delete;
  temporary_directory& operator=(const temporary_directory&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
};

void write_file(const std::filesystem::path& path, std::string_view content) {
  if (path.has_parent_path())
    std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output)
    throw std::runtime_error("unable to create test file: " + path.string());
  output.write(content.data(), static_cast<std::streamsize>(content.size()));
  if (!output)
    throw std::runtime_error("unable to write test file: " + path.string());
}

nlohmann::json plugin_manifest_document(
  std::string id = "com.example.tools", std::string entrypoint = "bin/tools") {
  return {
    { "schemaVersion", 1 },
    { "id", std::move(id) },
    { "version", "1.2.3" },
    { "runtimeApiVersion", "1.0" },
    { "entrypoint", { { "kind", "process" }, { "path", std::move(entrypoint) } } },
    { "capabilities", { "tools" } },
    { "permissions", { { "networkHosts", { "api.example.com" } } } },
  };
}

void write_plugin_package(const std::filesystem::path& root, std::string_view entrypoint_content,
  std::string id = "com.example.tools") {
  const auto manifest = plugin_manifest_document(std::move(id));
  write_file(
    root / std::filesystem::path(wuwe::runtime_host::plugin_manifest_filename), manifest.dump(2));
  write_file(root / "bin" / "tools", entrypoint_content);
}

#ifdef WUWE_RUNTIME_PLUGIN_FIXTURE_PATH
void write_process_plugin_package(const std::filesystem::path& root, std::string id,
  std::vector<std::string> capabilities = { "tools" },
  std::vector<std::string> environment_permissions = {}) {
  const auto source = std::filesystem::path(WUWE_RUNTIME_PLUGIN_FIXTURE_PATH);
  const auto relative_entrypoint = std::filesystem::path("bin") / source.filename();
  auto manifest = plugin_manifest_document(std::move(id), relative_entrypoint.generic_string());
  manifest["capabilities"] = std::move(capabilities);
  manifest["permissions"] = {
    { "environment", std::move(environment_permissions) },
  };
  write_file(
    root / std::filesystem::path(wuwe::runtime_host::plugin_manifest_filename), manifest.dump(2));
  std::filesystem::create_directories((root / relative_entrypoint).parent_path());
  std::filesystem::copy_file(
    source, root / relative_entrypoint, std::filesystem::copy_options::overwrite_existing);
  std::filesystem::permissions(
    root / relative_entrypoint, std::filesystem::status(source).permissions());
#ifdef _WIN32
  for (const auto& entry : std::filesystem::directory_iterator(source.parent_path())) {
    auto extension = entry.path().extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
      return static_cast<char>(std::tolower(ch));
    });
    if (entry.is_regular_file() && extension == ".dll") {
      std::filesystem::copy_file(entry.path(),
        root / "bin" / entry.path().filename(),
        std::filesystem::copy_options::overwrite_existing);
    }
  }
#endif
}
#endif

class fake_llm_client final : public wuwe::llm_client {
public:
  wuwe::llm_response complete(const wuwe::llm_request& request) override {
    return complete(request, {});
  }

  wuwe::llm_response complete(
    const wuwe::llm_request& request, std::stop_token stop_token) override {
    ++completion_count_;
    if (block_) {
      started_.set_value();
      while (!stop_token.stop_requested())
        std::this_thread::sleep_for(2ms);
      std::this_thread::sleep_for(cancellation_delay_);
      return { .error_code = wuwe::agent::make_error_code(wuwe::agent::llm_error_code::cancelled) };
    }
    const auto content =
      request.messages.empty() ? std::string {} : request.messages.back().content;
    return { .content = "completed: " + content, .stop_reason = "stop" };
  }

  void block_until_cancelled() {
    block_ = true;
    started_future_ = started_.get_future();
  }

  void delay_cancellation(std::chrono::milliseconds delay) {
    cancellation_delay_ = delay;
  }

  void stop_blocking() {
    block_ = false;
  }

  void wait_until_started() {
    expect(
      started_future_.wait_for(2s) == std::future_status::ready, "blocking fake LLM did not start");
  }

  [[nodiscard]] int completion_count() const noexcept {
    return completion_count_.load();
  }

private:
  bool block_ { false };
  std::promise<void> started_;
  std::future<void> started_future_;
  std::chrono::milliseconds cancellation_delay_ { 0 };
  std::atomic<int> completion_count_ { 0 };
};

class tool_inspecting_llm_client final : public wuwe::llm_client {
public:
  wuwe::llm_response complete(const wuwe::llm_request& request) override {
    observed_tools = request.tools;
    return { .content = "configured tools observed", .stop_reason = "stop" };
  }

  std::vector<wuwe::llm_tool> observed_tools;
};

class configured_plugin_tool_call_client final : public wuwe::llm_client {
public:
  wuwe::llm_response complete(const wuwe::llm_request& request) override {
    observed_tools = request.tools;
    ++calls;
    if (calls == 1) {
      return {
        .tool_calls = { {
          .id = "configured-plugin-call",
          .name = "runtime_echo",
          .arguments_json = R"({"text":"invoked through Runtime Host"})",
        } },
      };
    }
    saw_tool_result = !request.messages.empty() && request.messages.back().role == "tool" &&
                      request.messages.back().content == "invoked through Runtime Host";
    return { .content = "configured plugin completed", .stop_reason = "stop" };
  }

  int calls { 0 };
  bool saw_tool_result { false };
  std::vector<wuwe::llm_tool> observed_tools;
};

wuwe::runtime_host::runtime_dependencies dependencies_for(
  const std::shared_ptr<wuwe::llm_client>& client) {
  return {
    .make_llm_client = [client](const auto&) { return client; },
  };
}

void config_validation_is_strict_and_side_effect_free() {
  auto document = valid_config_document();
  expect(wuwe::runtime_host::validate_runtime_config(document).valid(),
    "valid runtime config should pass validation");

  document["llm"]["apiKey"] = "must-not-be-accepted";
  const auto secret = wuwe::runtime_host::validate_runtime_config(document);
  expect(!secret.valid(), "inline API keys must be rejected");
  expect(secret.issues.front().path == "/llm/apiKey",
    "inline secret rejection should identify the exact field path");

  document = valid_config_document();
  document["llm"]["provider"] = 7;
  const auto malformed = wuwe::runtime_host::validate_runtime_config(document);
  expect(!malformed.valid(), "wrongly typed fields must produce a validation report");

  document = valid_config_document();
  document["llm"]["baseUrl"] = "https://user:secret@example.com/v1";
  expect(!wuwe::runtime_host::validate_runtime_config(document).valid(),
    "baseUrl must not provide an alternate channel for inline credentials");
  document["llm"]["baseUrl"] = "https://api-.example.com/v1";
  expect(!wuwe::runtime_host::validate_runtime_config(document).valid(),
    "baseUrl must reject malformed DNS labels");
  document["llm"]["baseUrl"] = "https://example.com:443junk/v1";
  expect(!wuwe::runtime_host::validate_runtime_config(document).valid(),
    "baseUrl must reject malformed ports");
  document["llm"]["baseUrl"] = "http://[::1]:11434/v1";
  expect(wuwe::runtime_host::validate_runtime_config(document).valid(),
    "baseUrl should accept a well-formed bracketed IPv6 endpoint");

  const auto temp =
    std::filesystem::temp_directory_path() / "wuwe-runtime-validate-no-side-effect.db";
  std::error_code ignored;
  std::filesystem::remove(temp, ignored);
  document = valid_config_document();
  document["store"] = { { "kind", "sqlite" }, { "path", temp.string() } };
  (void)wuwe::runtime_host::runtime_config_from_json(document);
  expect(!std::filesystem::exists(temp), "config parsing must not create the SQLite database");

  auto typed = wuwe::runtime_host::runtime_config_from_json(valid_config_document());
  typed.store.sqlite_path = "must-not-be-hidden.db";
  expect(
    !wuwe::runtime_host::validate_runtime_config(wuwe::runtime_host::runtime_config_to_json(typed))
       .valid(),
    "typed configuration serialization must preserve inapplicable store paths for validation");
  typed.store.sqlite_path.clear();
  typed.store.kind = static_cast<wuwe::runtime_host::run_store_kind>(999);
  expect(
    !wuwe::runtime_host::validate_runtime_config(wuwe::runtime_host::runtime_config_to_json(typed))
       .valid(),
    "typed configuration serialization must not normalize unknown enum values");

  document = valid_config_document();
  document["plugins"] = {
    { "isolation", "trusted_process" },
    { "packages",
      { {
        { "path", "plugins/com.example.tools" },
        { "expectedId", "com.example.tools" },
        { "expectedContentSha256", std::string(64, 'a') },
        { "environment", { "PLUGIN_API_TOKEN" } },
        { "tools", { { { "name", "search" }, { "exposedName", "search_knowledge" } } } },
      } } },
  };
  expect(wuwe::runtime_host::validate_runtime_config(document).valid(),
    "explicit plugin package and tool configuration should be valid");
  const auto plugin_config = wuwe::runtime_host::runtime_config_from_json(document);
  expect(plugin_config.plugins.packages.size() == 1 &&
           plugin_config.plugins.packages.front().environment ==
             std::vector<std::string> { "PLUGIN_API_TOKEN" } &&
           plugin_config.plugins.packages.front().tools.front().exposed_name == "search_knowledge",
    "typed plugin configuration should preserve package-scoped grants and explicit bindings");
  expect(wuwe::runtime_host::validate_runtime_config(
           wuwe::runtime_host::runtime_config_to_json(plugin_config))
           .valid(),
    "typed plugin configuration should round-trip through the strict schema");

  auto invalid_plugins = document;
  invalid_plugins["plugins"]["packages"][0]["inlineSecret"] = "must-not-be-accepted";
  expect(!wuwe::runtime_host::validate_runtime_config(invalid_plugins).valid(),
    "plugin configuration must reject unknown inline secret fields");
  invalid_plugins = document;
  invalid_plugins["plugins"]["packages"][0]["tools"].push_back(
    { { "name", "other" }, { "exposedName", "search_knowledge" } });
  expect(!wuwe::runtime_host::validate_runtime_config(invalid_plugins).valid(),
    "exposed plugin tool names must be globally unique");
  invalid_plugins = document;
  invalid_plugins["plugins"]["environment"] = { "PLUGIN_API_TOKEN" };
  expect(!wuwe::runtime_host::validate_runtime_config(invalid_plugins).valid(),
    "plugin environment grants must not be accepted at host-global scope");
  invalid_plugins = document;
  invalid_plugins["plugins"]["packages"][0]["environment"] = { "WUWE_PLUGIN_ID" };
  expect(!wuwe::runtime_host::validate_runtime_config(invalid_plugins).valid(),
    "plugin configuration must reject Runtime-reserved environment grants");
}

void plugin_config_paths_are_resolved_from_the_config_file() {
  temporary_directory root("plugin-config-relative-path");
  auto document = valid_config_document();
  document["plugins"] = {
    { "isolation", "trusted_process" },
    { "packages",
      { {
        { "path", "plugins/com.example.tools" },
        { "expectedId", "com.example.tools" },
        { "expectedContentSha256", std::string(64, 'a') },
        { "tools", { { { "name", "echo_text" }, { "exposedName", "runtime_echo" } } } },
      } } },
  };
  const auto config_path = root.path() / "runtime.json";
  write_file(config_path, document.dump(2));
  const auto config = wuwe::runtime_host::load_runtime_config(config_path);
  expect(config.plugins.packages.front().path ==
           (root.path() / "plugins" / "com.example.tools").lexically_normal(),
    "plugin package paths must resolve relative to their configuration file");
}

void lifecycle_contract_is_explicit() {
  wuwe::runtime_host::lifecycle lifecycle;
  lifecycle.transition(wuwe::runtime_host::lifecycle_state::configured);
  lifecycle.transition(wuwe::runtime_host::lifecycle_state::starting);
  lifecycle.transition(wuwe::runtime_host::lifecycle_state::ready);
  lifecycle.transition(wuwe::runtime_host::lifecycle_state::draining);
  lifecycle.transition(wuwe::runtime_host::lifecycle_state::stopped);
  expect_throws<wuwe::runtime_host::lifecycle_error>(
    [&] { lifecycle.transition(wuwe::runtime_host::lifecycle_state::ready); },
    "terminal lifecycle states must reject transitions");
}

void plugin_manifest_rejects_native_loading() {
  const auto process_manifest = plugin_manifest_document();
  expect(wuwe::runtime_host::validate_plugin_manifest(process_manifest).valid(),
    "process plugin manifest should be valid");
  auto native_manifest = process_manifest;
  native_manifest["entrypoint"]["kind"] = "native_library";
  expect(!wuwe::runtime_host::validate_plugin_manifest(native_manifest).valid(),
    "native library plugin entrypoints must be rejected");
  auto escaping_manifest = process_manifest;
  escaping_manifest["entrypoint"]["path"] = "../outside/plugin";
  expect(!wuwe::runtime_host::validate_plugin_manifest(escaping_manifest).valid(),
    "plugin entrypoint must not escape its package");
  auto windows_escaping_manifest = process_manifest;
  windows_escaping_manifest["entrypoint"]["path"] = "..\\outside\\plugin.exe";
  expect(!wuwe::runtime_host::validate_plugin_manifest(windows_escaping_manifest).valid(),
    "plugin entrypoint traversal must be rejected independently of the host platform");
  auto reserved_manifest = process_manifest;
  reserved_manifest["entrypoint"]["path"] = "bin/con.exe";
  expect(!wuwe::runtime_host::validate_plugin_manifest(reserved_manifest).valid(),
    "plugin entrypoints must reject non-portable reserved path components");
  auto invalid_id = process_manifest;
  invalid_id["id"] = "com..example";
  expect(!wuwe::runtime_host::validate_plugin_manifest(invalid_id).valid(),
    "plugin ids must consist of valid DNS-style labels");
  invalid_id["id"] = std::string(64, 'a') + ".example";
  expect(!wuwe::runtime_host::validate_plugin_manifest(invalid_id).valid(),
    "plugin id labels must respect DNS length limits");
  auto semver_with_build = process_manifest;
  semver_with_build["version"] = "1.2.3-rc.1+build.7";
  expect(wuwe::runtime_host::validate_plugin_manifest(semver_with_build).valid(),
    "plugin versions should accept standard SemVer prerelease and build metadata");
  auto invalid_semver = process_manifest;
  invalid_semver["version"] = "1.2.3-01";
  expect(!wuwe::runtime_host::validate_plugin_manifest(invalid_semver).valid(),
    "numeric SemVer prerelease identifiers must not contain leading zeroes");
  auto invalid_port = process_manifest;
  invalid_port["permissions"]["networkHosts"] = { "api.example.com:65536" };
  expect(!wuwe::runtime_host::validate_plugin_manifest(invalid_port).valid(),
    "network permissions must reject ports outside the TCP/UDP range");
  auto invalid_host = process_manifest;
  invalid_host["permissions"]["networkHosts"] = { "api-.example.com" };
  expect(!wuwe::runtime_host::validate_plugin_manifest(invalid_host).valid(),
    "network permissions must reject malformed DNS labels");
  invalid_host["permissions"]["networkHosts"] = { "api.example.com:443junk" };
  expect(!wuwe::runtime_host::validate_plugin_manifest(invalid_host).valid(),
    "network permissions must reject ports with trailing characters");
  invalid_host["permissions"]["networkHosts"] = { "[::::]" };
  expect(!wuwe::runtime_host::validate_plugin_manifest(invalid_host).valid(),
    "network permissions must reject malformed IPv6 literals");
  auto reserved_environment = process_manifest;
  reserved_environment["permissions"]["environment"] = { "wuwe_plugin_id" };
  expect(!wuwe::runtime_host::validate_plugin_manifest(reserved_environment).valid(),
    "plugin environment permissions must not collide with Runtime metadata names");
  auto typed = wuwe::runtime_host::plugin_manifest_from_json(process_manifest);
  typed.entrypoint_kind = static_cast<wuwe::runtime_host::plugin_entrypoint_kind>(999);
  expect(!wuwe::runtime_host::validate_plugin_manifest(
           wuwe::runtime_host::plugin_manifest_to_json(typed))
            .valid(),
    "plugin serialization must not normalize an unknown entrypoint kind");
}

void plugin_packages_are_admitted_and_registered_by_verified_identity() {
  temporary_directory first_root("plugin-first");
  write_plugin_package(first_root.path(), "first executable bytes");
  const auto first = wuwe::runtime_host::load_plugin_package(first_root.path());
  expect(first->manifest().id == "com.example.tools", "admitted plugin should expose its manifest");
  expect(first->root() == std::filesystem::canonical(first_root.path()),
    "admitted plugin should expose a canonical package root");
  expect(
    first->entrypoint_path() == std::filesystem::canonical(first_root.path() / "bin" / "tools"),
    "admitted plugin should expose a canonical entrypoint path");
  expect(first->content_sha256().size() == 64,
    "admitted plugin should expose a stable SHA-256 package identity");

  wuwe::runtime_host::plugin_catalog catalog;
  const auto inserted = catalog.register_package(first);
  expect(inserted.status == wuwe::runtime_host::plugin_registration_status::inserted,
    "first package registration should insert the plugin");
  const auto stable_snapshot = catalog.snapshot();
  expect(stable_snapshot.find("com.example.tools") == first,
    "catalog snapshot should resolve the admitted plugin by id");
  expect(stable_snapshot.providing("tools") == std::vector { first },
    "catalog snapshot should resolve providers deterministically by declared capability");

  const auto same = wuwe::runtime_host::load_plugin_package(first_root.path());
  const auto unchanged = catalog.register_package(same);
  expect(unchanged.status == wuwe::runtime_host::plugin_registration_status::unchanged,
    "identical package content should be idempotent in the catalog");
  expect_throws<std::invalid_argument>(
    [&] {
      (void)catalog.register_package(
        same, static_cast<wuwe::runtime_host::plugin_registration_policy>(999));
    },
    "plugin catalog must reject unknown registration policies");

  temporary_directory replacement_root("plugin-replacement");
  write_plugin_package(replacement_root.path(), "replacement executable bytes");
  const auto replacement = wuwe::runtime_host::load_plugin_package(replacement_root.path());
  expect(replacement->content_sha256() != first->content_sha256(),
    "entrypoint changes must change the verified package identity");
  const auto conflict = catalog.register_package(replacement);
  expect(conflict.status == wuwe::runtime_host::plugin_registration_status::conflict &&
           conflict.previous == first,
    "different content for a registered plugin id should conflict by default");
  const auto replaced =
    catalog.register_package(replacement, wuwe::runtime_host::plugin_registration_policy::replace);
  expect(replaced.status == wuwe::runtime_host::plugin_registration_status::replaced &&
           catalog.snapshot().find("com.example.tools") == replacement,
    "explicit replacement should atomically publish the new package");
  expect(stable_snapshot.find("com.example.tools") == first,
    "existing catalog snapshots must remain immutable after replacement");
  expect(catalog.unregister_package("com.example.tools") && catalog.snapshot().empty(),
    "plugin catalog should support explicit unregistration");
}

void plugin_package_admission_rejects_missing_or_ambiguous_files() {
  temporary_directory missing_manifest("plugin-missing-manifest");
  expect_plugin_error(
    wuwe::runtime_host::plugin_package_error_code::manifest_unavailable,
    [&] { (void)wuwe::runtime_host::load_plugin_package(missing_manifest.path()); },
    "plugin admission must classify a missing manifest");

  temporary_directory invalid_manifest("plugin-invalid-manifest");
  write_file(
    invalid_manifest.path() / std::filesystem::path(wuwe::runtime_host::plugin_manifest_filename),
    "{not-json");
  expect_plugin_error(
    wuwe::runtime_host::plugin_package_error_code::manifest_invalid,
    [&] { (void)wuwe::runtime_host::load_plugin_package(invalid_manifest.path()); },
    "plugin admission must classify invalid manifest content");

  temporary_directory missing_entrypoint("plugin-missing-entrypoint");
  write_file(
    missing_entrypoint.path() / std::filesystem::path(wuwe::runtime_host::plugin_manifest_filename),
    plugin_manifest_document().dump(2));
  expect_plugin_error(
    wuwe::runtime_host::plugin_package_error_code::entrypoint_unavailable,
    [&] { (void)wuwe::runtime_host::load_plugin_package(missing_entrypoint.path()); },
    "plugin admission must reject a missing entrypoint");

  temporary_directory directory_entrypoint("plugin-directory-entrypoint");
  write_file(directory_entrypoint.path() /
               std::filesystem::path(wuwe::runtime_host::plugin_manifest_filename),
    plugin_manifest_document().dump(2));
  std::filesystem::create_directories(directory_entrypoint.path() / "bin" / "tools");
  expect_plugin_error(
    wuwe::runtime_host::plugin_package_error_code::unsupported_file,
    [&] { (void)wuwe::runtime_host::load_plugin_package(directory_entrypoint.path()); },
    "plugin admission must reject a directory in place of an executable file");

  temporary_directory oversized_manifest("plugin-oversized-manifest");
  write_file(
    oversized_manifest.path() / std::filesystem::path(wuwe::runtime_host::plugin_manifest_filename),
    std::string(1024 * 1024 + 1, 'x'));
  expect_plugin_error(
    wuwe::runtime_host::plugin_package_error_code::manifest_too_large,
    [&] { (void)wuwe::runtime_host::load_plugin_package(oversized_manifest.path()); },
    "plugin admission must reject an oversized manifest before parsing it");
}

void plugin_package_admission_rejects_link_aliases() {
  temporary_directory workspace("plugin-link-aliases");
  const auto outside = workspace.path() / "outside";
  write_file(outside, "external executable bytes");

  const auto symlink_root = workspace.path() / "symlink-package";
  std::filesystem::create_directories(symlink_root / "bin");
  write_file(symlink_root / std::filesystem::path(wuwe::runtime_host::plugin_manifest_filename),
    plugin_manifest_document().dump(2));
  std::error_code link_error;
  std::filesystem::create_symlink(outside, symlink_root / "bin" / "tools", link_error);
  if (!link_error) {
    expect_plugin_error(
      wuwe::runtime_host::plugin_package_error_code::unsafe_path,
      [&] { (void)wuwe::runtime_host::load_plugin_package(symlink_root); },
      "plugin admission must reject symbolic-link entrypoints");
  }

  const auto hardlink_root = workspace.path() / "hardlink-package";
  std::filesystem::create_directories(hardlink_root / "bin");
  write_file(hardlink_root / std::filesystem::path(wuwe::runtime_host::plugin_manifest_filename),
    plugin_manifest_document().dump(2));
  link_error.clear();
  std::filesystem::create_hard_link(outside, hardlink_root / "bin" / "tools", link_error);
  if (!link_error) {
    expect_plugin_error(
      wuwe::runtime_host::plugin_package_error_code::unsafe_path,
      [&] { (void)wuwe::runtime_host::load_plugin_package(hardlink_root); },
      "plugin admission must reject hard-linked entrypoints");
  }
}

void plugin_process_execution_is_explicit_and_capability_gated() {
#ifdef WUWE_RUNTIME_PLUGIN_FIXTURE_PATH
  temporary_directory package_root("plugin-process");
  write_process_plugin_package(package_root.path(), "com.example.process");
  const auto package = wuwe::runtime_host::load_plugin_package(package_root.path());

  wuwe::runtime_host::plugin_process_supervisor sandboxed;
  expect_plugin_process_error(
    wuwe::runtime_host::plugin_process_error_code::invalid_options,
    [&] { (void)sandboxed.start({}); },
    "plugin process start must reject a null package");
  expect_plugin_process_error(
    wuwe::runtime_host::plugin_process_error_code::isolation_unavailable,
    [&] { (void)sandboxed.start(package); },
    "default plugin process isolation must fail closed when enforcement is unavailable");

  wuwe::runtime_host::plugin_process_supervisor trusted({
    .isolation = wuwe::runtime_host::plugin_process_isolation::trusted_process,
  });
  const auto started = trusted.start(package);
  expect(started.state == wuwe::runtime_host::plugin_process_state::running,
    "trusted plugin process should reach running state");
  expect(!started.permissions_enforced,
    "trusted plugin process must not claim filesystem or network enforcement");
  expect(trusted.snapshots().size() == 1,
    "plugin process supervisor should expose loaded process snapshots");
  expect_plugin_process_error(
    wuwe::runtime_host::plugin_process_error_code::already_loaded,
    [&] { (void)trusted.start(package); },
    "plugin process ids must be unique while loaded");

  const auto tools = trusted.list_tools(package->manifest().id);
  expect(
    tools.at("tools").size() == 2, "plugin process should expose tools from its MCP entrypoint");
  const auto echoed =
    trusted.call_tool(package->manifest().id, "echo_text", { { "text", "runtime plugin" } });
  expect(echoed.at("content").at(0).at("text") == "runtime plugin",
    "plugin process should route MCP tool calls");

  expect(trusted.stop(package->manifest().id), "loaded plugin process should stop");
  expect(trusted.snapshot(package->manifest().id).state ==
           wuwe::runtime_host::plugin_process_state::stopped,
    "stopped plugin process should remain inspectable until unloaded");
  expect_plugin_process_error(
    wuwe::runtime_host::plugin_process_error_code::not_running,
    [&] { (void)trusted.list_tools(package->manifest().id); },
    "tool operations on a stopped plugin must have a stable not-running classification");
  expect(trusted.unload(package->manifest().id), "stopped plugin process should unload");
  expect(!trusted.unload(package->manifest().id), "unloading an unknown process should be false");
  expect_plugin_process_error(
    wuwe::runtime_host::plugin_process_error_code::not_found,
    [&] { (void)trusted.snapshot(package->manifest().id); },
    "unloaded plugin process should no longer have a snapshot");

  temporary_directory no_tools_root("plugin-process-no-tools");
  write_process_plugin_package(no_tools_root.path(), "com.example.no-tools", {});
  const auto no_tools = wuwe::runtime_host::load_plugin_package(no_tools_root.path());
  (void)trusted.start(no_tools);
  expect_plugin_process_error(
    wuwe::runtime_host::plugin_process_error_code::capability_denied,
    [&] { (void)trusted.list_tools(no_tools->manifest().id); },
    "undeclared tool capability must deny tool discovery");
  expect_plugin_process_error(
    wuwe::runtime_host::plugin_process_error_code::capability_denied,
    [&] { (void)trusted.call_tool(no_tools->manifest().id, "echo_text"); },
    "undeclared tool capability must deny tool invocation");
#endif
}

void plugin_process_environment_is_allowlisted_and_packages_are_revalidated() {
#ifdef WUWE_RUNTIME_PLUGIN_FIXTURE_PATH
  temporary_directory package_root("plugin-process-environment");
  write_process_plugin_package(
    package_root.path(), "com.example.environment", { "tools" }, { "WUWE_RUNTIME_GRANTED_TEST" });
  const auto package = wuwe::runtime_host::load_plugin_package(package_root.path());

  wuwe::runtime_host::plugin_process_supervisor missing_environment({
    .isolation = wuwe::runtime_host::plugin_process_isolation::trusted_process,
  });
  expect_plugin_process_error(
    wuwe::runtime_host::plugin_process_error_code::environment_unavailable,
    [&] { (void)missing_environment.start(package); },
    "manifest environment requirements must have an explicit host grant");

#ifdef _WIN32
  expect(_putenv_s("WUWE_RUNTIME_PARENT_ONLY_TEST", "must-not-leak") == 0,
    "test should set a parent-only environment value");
#else
  expect(setenv("WUWE_RUNTIME_PARENT_ONLY_TEST", "must-not-leak", 1) == 0,
    "test should set a parent-only environment value");
#endif
  wuwe::runtime_host::plugin_process_supervisor trusted({
    .isolation = wuwe::runtime_host::plugin_process_isolation::trusted_process,
    .environment = { { "WUWE_RUNTIME_GRANTED_TEST", "granted-value" } },
  });
  (void)trusted.start(package);
  const auto granted = trusted.call_tool(
    package->manifest().id, "environment_value", { { "name", "WUWE_RUNTIME_GRANTED_TEST" } });
  const auto inherited = trusted.call_tool(
    package->manifest().id, "environment_value", { { "name", "WUWE_RUNTIME_PARENT_ONLY_TEST" } });
#ifdef _WIN32
  (void)_putenv_s("WUWE_RUNTIME_PARENT_ONLY_TEST", "");
#else
  (void)unsetenv("WUWE_RUNTIME_PARENT_ONLY_TEST");
#endif
  expect(granted.at("content").at(0).value("text", std::string {}) == "granted-value",
    "explicit environment grant should reach the plugin process");
  expect(inherited.at("content").at(0).value("text", std::string {}).empty(),
    "parent environment should not leak into the plugin process");

  temporary_directory changed_root("plugin-process-changed");
  write_process_plugin_package(changed_root.path(), "com.example.changed");
  const auto changed = wuwe::runtime_host::load_plugin_package(changed_root.path());
  write_file(changed->entrypoint_path(), "replaced after admission");
  expect_plugin_process_error(
    wuwe::runtime_host::plugin_process_error_code::package_changed,
    [&] { (void)trusted.start(changed); },
    "plugin package identity must be revalidated immediately before launch");

  expect_plugin_process_error(
    wuwe::runtime_host::plugin_process_error_code::invalid_options,
    [] {
      wuwe::runtime_host::plugin_process_supervisor invalid({
        .isolation = static_cast<wuwe::runtime_host::plugin_process_isolation>(999),
      });
    },
    "unknown plugin process isolation values must be rejected");
#endif
}

void plugin_activation_environment_is_scoped_per_package() {
#ifdef WUWE_RUNTIME_PLUGIN_FIXTURE_PATH
  constexpr std::string_view environment_name = "WUWE_RUNTIME_PACKAGE_SCOPED_TEST";
  temporary_directory first_root("plugin-environment-first");
  temporary_directory second_root("plugin-environment-second");
  write_process_plugin_package(first_root.path(),
    "com.example.environment-first",
    { "tools" },
    { std::string(environment_name) });
  write_process_plugin_package(second_root.path(),
    "com.example.environment-second",
    { "tools" },
    { std::string(environment_name) });

  wuwe::runtime_host::plugin_runtime runtime({
    .process_options = {
      .isolation = wuwe::runtime_host::plugin_process_isolation::trusted_process,
    },
  });
  (void)runtime.register_package(first_root.path());
  (void)runtime.register_package(second_root.path());
  (void)runtime.activate("com.example.environment-first",
    { .environment = { { std::string(environment_name), "first-secret" } } });
  (void)runtime.activate("com.example.environment-second",
    { .environment = { { std::string(environment_name), "second-secret" } } });

  const auto first = runtime.call_tool(
    "com.example.environment-first", "environment_value", { { "name", environment_name } });
  const auto second = runtime.call_tool(
    "com.example.environment-second", "environment_value", { { "name", environment_name } });
  expect(first.at("content").at(0).value("text", std::string {}) == "first-secret" &&
           second.at("content").at(0).value("text", std::string {}) == "second-secret",
    "per-activation environment grants must remain scoped to their plugin process");
  expect(runtime.deactivate("com.example.environment-first") &&
           runtime.deactivate("com.example.environment-second"),
    "package-scoped environment fixtures should stop cleanly");
#endif
}

void plugin_runtime_coordinates_catalog_and_process_lifecycle() {
#ifdef WUWE_RUNTIME_PLUGIN_FIXTURE_PATH
  temporary_directory first_root("plugin-runtime-first");
  write_process_plugin_package(first_root.path(), "com.example.runtime");

  wuwe::runtime_host::plugin_runtime sandboxed;
  expect_throws<std::invalid_argument>(
    [&] { (void)sandboxed.register_package(wuwe::runtime_host::plugin_package_ptr {}); },
    "plugin runtime should reject null package registration");
  (void)sandboxed.register_package(first_root.path());
  expect_plugin_process_error(
    wuwe::runtime_host::plugin_process_error_code::isolation_unavailable,
    [&] { (void)sandboxed.activate("com.example.runtime"); },
    "plugin runtime should preserve the default fail-closed process policy");

  wuwe::runtime_host::plugin_runtime runtime({
    .process_options = {
      .isolation = wuwe::runtime_host::plugin_process_isolation::trusted_process,
    },
  });
  const auto inserted = runtime.register_package(first_root.path());
  expect(inserted.status == wuwe::runtime_host::plugin_registration_status::inserted,
    "plugin runtime should publish an admitted package");
  expect(runtime.catalog_snapshot().size() == 1,
    "plugin runtime should expose its immutable catalog snapshot");

  const auto activated = runtime.activate("com.example.runtime");
  expect(activated.state == wuwe::runtime_host::plugin_process_state::running,
    "plugin runtime should activate its registered package");
  const auto activated_again = runtime.activate("com.example.runtime");
  expect(activated_again.content_sha256 == activated.content_sha256 &&
           runtime.process_snapshots().size() == 1,
    "plugin runtime activation should be idempotent while running");
  const auto echoed =
    runtime.call_tool("com.example.runtime", "echo_text", { { "text", "coordinated" } });
  expect(echoed.at("content").at(0).at("text") == "coordinated",
    "plugin runtime should route calls through its active process");

  const auto unchanged = runtime.register_package(first_root.path());
  expect(unchanged.status == wuwe::runtime_host::plugin_registration_status::unchanged &&
           runtime.process_snapshots().size() == 1,
    "idempotent registration must not disturb an active process");

  temporary_directory replacement_root("plugin-runtime-replacement");
  write_process_plugin_package(replacement_root.path(), "com.example.runtime");
  auto replacement_manifest = plugin_manifest_document("com.example.runtime",
    (std::filesystem::path("bin") /
      std::filesystem::path(WUWE_RUNTIME_PLUGIN_FIXTURE_PATH).filename())
      .generic_string());
  replacement_manifest["version"] = "2.0.0";
  replacement_manifest["permissions"] = nlohmann::json::object();
  write_file(
    replacement_root.path() / std::filesystem::path(wuwe::runtime_host::plugin_manifest_filename),
    replacement_manifest.dump(2));
  const auto replacement = wuwe::runtime_host::load_plugin_package(replacement_root.path());

  const auto conflict = runtime.register_package(replacement);
  expect(conflict.status == wuwe::runtime_host::plugin_registration_status::conflict &&
           runtime.process_snapshots().size() == 1,
    "rejected package conflicts must leave the active process untouched");
  expect_throws<std::invalid_argument>(
    [&] {
      (void)runtime.register_package(
        replacement, static_cast<wuwe::runtime_host::plugin_registration_policy>(999));
    },
    "invalid replacement policy must be rejected before changing process state");
  expect(runtime.process_snapshots().size() == 1,
    "invalid replacement policy must not deactivate the current process");

  const auto replaced =
    runtime.register_package(replacement, wuwe::runtime_host::plugin_registration_policy::replace);
  expect(replaced.status == wuwe::runtime_host::plugin_registration_status::replaced &&
           runtime.process_snapshots().empty(),
    "explicit replacement should deactivate the old process before publishing new content");
  expect(runtime.catalog_snapshot().find("com.example.runtime")->manifest().version == "2.0.0",
    "plugin runtime catalog should publish the explicit replacement");
  expect(runtime.activate("com.example.runtime").version == "2.0.0",
    "plugin runtime should activate the replacement package");

  expect(runtime.deactivate("com.example.runtime"),
    "plugin runtime should explicitly deactivate a running package");
  expect(runtime.process_snapshots().empty() && runtime.catalog_snapshot().size() == 1,
    "deactivation should preserve package registration");
  expect(runtime.activate("com.example.runtime").state ==
           wuwe::runtime_host::plugin_process_state::running,
    "a deactivated package should be activatable again");
  expect(runtime.remove_package("com.example.runtime"),
    "package removal should deactivate and unregister the plugin");
  expect(runtime.catalog_snapshot().empty() && runtime.process_snapshots().empty(),
    "package removal should leave no catalog or process state");
  expect(
    !runtime.remove_package("com.example.runtime") && !runtime.deactivate("com.example.runtime"),
    "removal and deactivation should be idempotent for unknown IDs");
  expect_plugin_runtime_error(
    wuwe::runtime_host::plugin_runtime_error_code::not_registered,
    [&] { (void)runtime.activate("com.example.runtime"); },
    "activation should classify an unregistered plugin ID");
#endif
}

void plugin_tools_are_explicitly_bound_into_the_tool_contract() {
#ifdef WUWE_RUNTIME_PLUGIN_FIXTURE_PATH
  temporary_directory package_root("plugin-tools");
  const std::string plugin_id = "com.example.plugin-tools";
  write_process_plugin_package(package_root.path(), plugin_id);

  wuwe::runtime_host::plugin_runtime runtime({
    .process_options = {
      .isolation = wuwe::runtime_host::plugin_process_isolation::trusted_process,
    },
  });
  (void)runtime.register_package(package_root.path());

  expect_throws<std::invalid_argument>(
    [&] {
      wuwe::runtime_host::plugin_tool_provider inactive(runtime,
        { {
          .plugin_id = plugin_id,
          .tool_name = "echo_text",
        } });
    },
    "plugin tool bindings should require an active plugin for schema discovery");
  expect_throws<std::invalid_argument>(
    [&] {
      wuwe::runtime_host::plugin_tool_provider missing(runtime,
        { {
          .plugin_id = "com.example.missing",
          .tool_name = "echo_text",
        } });
    },
    "plugin tool bindings should reject an unknown plugin");

  (void)runtime.activate(plugin_id);
  auto provider = std::make_shared<wuwe::runtime_host::plugin_tool_provider>(runtime,
    std::vector<wuwe::runtime_host::plugin_tool_binding> {
      {
        .plugin_id = plugin_id,
        .tool_name = "echo_text",
      },
    });
  const auto descriptors = provider->descriptors();
  expect(descriptors.size() == 1 && descriptors.front().name == plugin_id + "__echo_text",
    "plugin tools should expose only explicitly selected bindings with deterministic names");
  const auto& descriptor = descriptors.front();
  expect(descriptor.description == "Echo a string for Runtime plugin process tests." &&
           descriptor.input_schema.at("properties").contains("text"),
    "plugin tool descriptors should preserve the MCP description and input schema");
  expect(descriptor.version == "1.2.3" &&
           descriptor.metadata.at("wuwe.runtime.plugin.id") == plugin_id &&
           descriptor.metadata.at("wuwe.runtime.plugin.version") == "1.2.3" &&
           descriptor.metadata.at("wuwe.runtime.plugin.content_sha256").size() == 64 &&
           descriptor.metadata.at("wuwe.runtime.plugin.tool") == "echo_text",
    "plugin tool descriptors should retain the admitted package identity");
  expect(descriptor.approval == wuwe::agent::tools::tool_approval_mode::policy &&
           descriptor.side_effect == wuwe::agent::tools::tool_side_effect::write &&
           descriptor.capabilities.size() == 1 &&
           descriptor.capabilities.front().name == "runtime.plugin.tool.call" &&
           descriptor.capabilities.front().risk ==
             wuwe::agent::capability::capability_risk_level::high &&
           descriptor.capabilities.front().resources ==
             std::vector<std::string> { plugin_id + ":echo_text" },
    "plugin tool bindings should retain policy approval and a high-risk scoped capability");
  expect(
    provider->tools().size() == 1 && provider->tools().front().name == plugin_id + "__echo_text",
    "plugin tool providers should satisfy the model-facing provider interface");

  fake_llm_client client;
  wuwe::llm_agent_runner runner(client, provider, 0);
  (void)runner;

  const auto echoed = provider->invoke(plugin_id + "__echo_text", R"({"text":"bound"})");
  expect(echoed.succeeded() && echoed.content == "bound",
    "an explicitly bound plugin tool should reach the selected process");
  expect(provider->invoke("unbound", "{}").error_category ==
           wuwe::agent::tools::tool_error_category::not_found,
    "unbound plugin tools should not be invokable");
  expect(provider->invoke(plugin_id + "__echo_text", "{").error_category ==
           wuwe::agent::tools::tool_error_category::invalid_input,
    "malformed plugin tool arguments should have a stable invalid-input classification");

  std::stop_source stop;
  stop.request_stop();
  expect(provider
             ->invoke({
               .name = plugin_id + "__echo_text",
               .arguments_json = R"({"text":"cancelled"})",
               .stop_token = stop.get_token(),
             })
             .error_category == wuwe::agent::tools::tool_error_category::cancelled,
    "plugin tool bindings should honor cancellation before synchronous dispatch");

  wuwe::runtime_host::plugin_tool_provider custom(runtime,
    { {
      .plugin_id = plugin_id,
      .tool_name = "environment_value",
      .exposed_name = "read_plugin_environment",
    } });
  expect(custom.descriptors().size() == 1 &&
           custom.descriptors().front().name == "read_plugin_environment",
    "plugin tool bindings should support an explicit public name");
  expect_throws<std::invalid_argument>(
    [&] {
      wuwe::runtime_host::plugin_tool_provider duplicate(runtime,
        {
          {
            .plugin_id = plugin_id,
            .tool_name = "echo_text",
            .exposed_name = "duplicate",
          },
          {
            .plugin_id = plugin_id,
            .tool_name = "environment_value",
            .exposed_name = "duplicate",
          },
        });
    },
    "plugin tool bindings should reject duplicate exposed names");

  expect(runtime.deactivate(plugin_id), "plugin tool test should deactivate its process");
  expect(provider->invoke(plugin_id + "__echo_text", R"({"text":"stopped"})").error_category ==
           wuwe::agent::tools::tool_error_category::unavailable,
    "calls through a provider should report unavailable after plugin deactivation");
  (void)runtime.activate(plugin_id);
  expect(provider->invoke(plugin_id + "__echo_text", R"({"text":"reactivated"})").content ==
           "reactivated",
    "a provider should remain valid when the same admitted package is reactivated");

  temporary_directory replacement_root("plugin-tools-replacement");
  write_process_plugin_package(replacement_root.path(), plugin_id);
  auto replacement_manifest = plugin_manifest_document(plugin_id,
    (std::filesystem::path("bin") /
      std::filesystem::path(WUWE_RUNTIME_PLUGIN_FIXTURE_PATH).filename())
      .generic_string());
  replacement_manifest["version"] = "2.0.0";
  replacement_manifest["permissions"] = nlohmann::json::object();
  write_file(
    replacement_root.path() / std::filesystem::path(wuwe::runtime_host::plugin_manifest_filename),
    replacement_manifest.dump(2));
  const auto replaced = runtime.register_package(
    replacement_root.path(), wuwe::runtime_host::plugin_registration_policy::replace);
  expect(replaced.status == wuwe::runtime_host::plugin_registration_status::replaced,
    "plugin tool identity test should publish explicit replacement content");
  (void)runtime.activate(plugin_id);
  expect(provider->invoke(plugin_id + "__echo_text", R"({"text":"replacement"})").error_category ==
           wuwe::agent::tools::tool_error_category::unavailable,
    "a provider must not dispatch its retained schema to replacement plugin content");
  expect(runtime.deactivate(plugin_id), "plugin tool identity test should stop its replacement");
#endif
}

void runtime_host_activates_only_configured_plugin_tools() {
#ifdef WUWE_RUNTIME_PLUGIN_FIXTURE_PATH
  temporary_directory package_root("configured-runtime-plugin");
  const std::string plugin_id = "com.example.configured";
  write_process_plugin_package(package_root.path(), plugin_id);
  const auto package = wuwe::runtime_host::load_plugin_package(package_root.path());

  auto document = valid_config_document();
  document["plugins"] = {
    { "isolation", "trusted_process" },
    { "packages",
      { {
        { "path", package_root.path().generic_string() },
        { "expectedId", plugin_id },
        { "expectedContentSha256", package->content_sha256() },
        { "tools", { { { "name", "echo_text" }, { "exposedName", "runtime_echo" } } } },
      } } },
  };
  const auto config = wuwe::runtime_host::runtime_config_from_json(document);
  auto client = std::make_shared<tool_inspecting_llm_client>();
  const auto dependencies = dependencies_for(client);
  const auto doctor = wuwe::runtime_host::diagnose_runtime_config(config, dependencies);
  expect(doctor.ready && doctor.checks.at("plugins").ok,
    "doctor should verify configured trusted plugin identity without launching it");

  wuwe::runtime_host::host host(dependencies);
  host.configure(config);
  host.start();
  expect(host.health().checks.at("plugins").ok,
    "configured plugin processes should participate in Runtime health");
  const auto result = host.run("inspect configured tools");
  expect(result.status == wuwe::agent::runtime::agent_run_status::completed,
    "configured plugin tools should compose into a normal Runtime run");
  expect(
    client->observed_tools.size() == 1 && client->observed_tools.front().name == "runtime_echo",
    "the default runner must expose only the explicitly configured plugin tool name");
  expect(host.stop(), "configured plugin runtime should stop cleanly");

  auto custom_client = std::make_shared<tool_inspecting_llm_client>();
  auto custom_dependencies = dependencies_for(custom_client);
  bool context_received = false;
  custom_dependencies.make_configured_agent_runner =
    [&](wuwe::llm_client& configured_client,
      const wuwe::runtime_host::runtime_runner_context& context) {
      context_received = context.tool_provider && context.tool_provider->tools().size() == 1;
      return std::make_shared<wuwe::llm_agent_runner>(configured_client, context.tool_provider);
    };
  wuwe::runtime_host::host custom_host(std::move(custom_dependencies));
  custom_host.configure(config);
  custom_host.start();
  expect(context_received,
    "context-aware runner assembly must receive the host-authorized tool provider");
  expect(custom_host.run("inspect custom runner tools").status ==
             wuwe::agent::runtime::agent_run_status::completed &&
           custom_client->observed_tools.size() == 1,
    "a custom runner assembler should be able to retain configured plugin tools");
  expect(custom_host.stop(), "context-aware custom runner should stop cleanly");

  auto manual_client = std::make_shared<configured_plugin_tool_call_client>();
  wuwe::runtime_host::host manual_host(dependencies_for(manual_client));
  manual_host.configure(config);
  manual_host.start();
  const auto suspended = manual_host.run("call the configured plugin");
  expect(suspended.status == wuwe::agent::runtime::agent_run_status::waiting_for_approval &&
           manual_client->calls == 1,
    "configured plugin calls must suspend for explicit approval when no policy is injected");
  expect(manual_host.stop(), "manual-approval plugin runtime should stop cleanly");

  auto approved_client = std::make_shared<configured_plugin_tool_call_client>();
  auto approved_dependencies = dependencies_for(approved_client);
  approved_dependencies.approval_service =
    std::make_shared<wuwe::agent::approval::allow_all_approval_service>();
  wuwe::runtime_host::host approved_host(std::move(approved_dependencies));
  approved_host.configure(config);
  approved_host.start();
  const auto approved = approved_host.run("call the configured plugin");
  expect(approved.status == wuwe::agent::runtime::agent_run_status::completed &&
           approved.content == "configured plugin completed" && approved_client->calls == 2 &&
           approved_client->saw_tool_result,
    "an explicitly injected approval policy must execute the configured plugin end to end");
  expect(approved_host.stop(), "approved plugin runtime should stop cleanly");
#endif
}

void runtime_host_plugin_activation_fails_closed() {
#ifdef WUWE_RUNTIME_PLUGIN_FIXTURE_PATH
  temporary_directory package_root("configured-runtime-plugin-failures");
  const std::string plugin_id = "com.example.failclosed";
  write_process_plugin_package(package_root.path(), plugin_id);
  const auto package = wuwe::runtime_host::load_plugin_package(package_root.path());
  auto document = valid_config_document();
  document["plugins"] = {
    { "isolation", "trusted_process" },
    { "packages",
      { {
        { "path", package_root.path().generic_string() },
        { "expectedId", plugin_id },
        { "expectedContentSha256", package->content_sha256() },
        { "tools", { { { "name", "echo_text" }, { "exposedName", "runtime_echo" } } } },
      } } },
  };

  auto missing_environment = document;
  missing_environment["plugins"]["packages"][0]["environment"] = {
    "WUWE_RUNTIME_PLUGIN_ENV_THAT_MUST_NOT_EXIST_9A1D",
  };
  const auto missing_environment_config =
    wuwe::runtime_host::runtime_config_from_json(missing_environment);
  expect(!wuwe::runtime_host::diagnose_runtime_config(
           missing_environment_config, dependencies_for(std::make_shared<fake_llm_client>()))
            .checks.at("plugins")
            .ok,
    "doctor must report missing plugin environment grants");
  wuwe::runtime_host::host missing_environment_host(
    dependencies_for(std::make_shared<fake_llm_client>()));
  missing_environment_host.configure(missing_environment_config);
  expect_throws<wuwe::runtime_host::runtime_error>([&] { missing_environment_host.start(); },
    "startup must fail when a configured plugin environment reference is unavailable");

  temporary_directory required_environment_root("configured-runtime-plugin-required-environment");
  const std::string required_environment_id = "com.example.required-environment";
  write_process_plugin_package(required_environment_root.path(),
    required_environment_id,
    { "tools" },
    { "WUWE_RUNTIME_PLUGIN_REQUIRED_ENVIRONMENT_31B7" });
  const auto required_environment_package =
    wuwe::runtime_host::load_plugin_package(required_environment_root.path());
  auto missing_grant = document;
  auto& missing_grant_package = missing_grant["plugins"]["packages"][0];
  missing_grant_package["path"] = required_environment_root.path().generic_string();
  missing_grant_package["expectedId"] = required_environment_id;
  missing_grant_package["expectedContentSha256"] = required_environment_package->content_sha256();
  const auto missing_grant_config = wuwe::runtime_host::runtime_config_from_json(missing_grant);
  expect(!wuwe::runtime_host::diagnose_runtime_config(
           missing_grant_config, dependencies_for(std::make_shared<fake_llm_client>()))
            .checks.at("plugins")
            .ok,
    "doctor must reject manifest environment requirements omitted from package grants");
  wuwe::runtime_host::host missing_grant_host(
    dependencies_for(std::make_shared<fake_llm_client>()));
  missing_grant_host.configure(missing_grant_config);
  expect_throws<wuwe::runtime_host::runtime_error>([&] { missing_grant_host.start(); },
    "startup must fail before launch when a manifest environment requirement is not granted");

  auto changed_hash = document;
  changed_hash["plugins"]["packages"][0]["expectedContentSha256"] = std::string(64, '0');
  const auto changed_hash_config = wuwe::runtime_host::runtime_config_from_json(changed_hash);
  expect(!wuwe::runtime_host::diagnose_runtime_config(
           changed_hash_config, dependencies_for(std::make_shared<fake_llm_client>()))
            .checks.at("plugins")
            .ok,
    "doctor must reject plugin package identity mismatches");
  wuwe::runtime_host::host changed_hash_host(dependencies_for(std::make_shared<fake_llm_client>()));
  changed_hash_host.configure(changed_hash_config);
  expect_throws<wuwe::runtime_host::runtime_error>([&] { changed_hash_host.start(); },
    "startup must reject plugin content that does not match its pinned SHA-256");

  auto sandboxed = document;
  sandboxed["plugins"]["isolation"] = "sandboxed_process";
  const auto sandboxed_config = wuwe::runtime_host::runtime_config_from_json(sandboxed);
  expect(!wuwe::runtime_host::diagnose_runtime_config(
           sandboxed_config, dependencies_for(std::make_shared<fake_llm_client>()))
            .checks.at("plugins")
            .ok,
    "doctor must not claim unavailable sandbox enforcement");
  wuwe::runtime_host::host sandboxed_host(dependencies_for(std::make_shared<fake_llm_client>()));
  sandboxed_host.configure(sandboxed_config);
  expect_throws<wuwe::runtime_host::runtime_error>([&] { sandboxed_host.start(); },
    "sandboxed plugin activation must fail closed until enforcement exists");

  auto legacy_dependencies = dependencies_for(std::make_shared<fake_llm_client>());
  legacy_dependencies.make_agent_runner = [](wuwe::llm_client& client) {
    return std::make_shared<wuwe::llm_agent_runner>(client);
  };
  const auto configured = wuwe::runtime_host::runtime_config_from_json(document);
  expect(!wuwe::runtime_host::diagnose_runtime_config(configured, legacy_dependencies)
            .checks.at("runner")
            .ok,
    "doctor must reject legacy custom runner assembly that cannot receive configured tools");
  wuwe::runtime_host::host legacy_host(std::move(legacy_dependencies));
  legacy_host.configure(configured);
  expect_throws<wuwe::runtime_host::runtime_error>([&] { legacy_host.start(); },
    "startup must not silently discard configured plugin tools in a custom runner");
#endif
}

void provider_specific_endpoint_requirements_are_preflighted() {
  auto document = valid_config_document();
  document["llm"]["provider"] = "OpenAICompatible";
  const auto config = wuwe::runtime_host::runtime_config_from_json(document);
  const auto report = wuwe::runtime_host::diagnose_runtime_config(config);
  expect(!report.checks.at("endpoint").ok,
    "preflight must reject a built-in provider whose required endpoint is missing");
  const auto dependencies = dependencies_for(std::make_shared<fake_llm_client>());
  expect(wuwe::runtime_host::diagnose_runtime_config(config, dependencies).ready,
    "a custom client factory must own its endpoint policy");
  wuwe::runtime_host::host host;
  host.configure(config);
  try {
    host.start();
    throw std::runtime_error("missing provider endpoint should have failed startup");
  }
  catch (const wuwe::runtime_host::runtime_error& ex) {
    expect(ex.code() == wuwe::runtime_host::runtime_error_code::dependency_unavailable,
      "missing provider endpoints must have a stable dependency error classification");
  }
}

void configured_credential_reference_is_authoritative() {
  auto document = valid_config_document();
  document["llm"]["apiKeyEnv"] = "WUWE_RUNTIME_TEST_KEY_THAT_MUST_NOT_EXIST_6F5A1D";
  const auto config = wuwe::runtime_host::runtime_config_from_json(document);
  expect(!wuwe::runtime_host::diagnose_runtime_config(config).checks.at("credential").ok,
    "preflight must report the configured missing credential reference");
  wuwe::runtime_host::host host;
  host.configure(config);
  try {
    host.start();
    throw std::runtime_error("missing configured credential should have failed startup");
  }
  catch (const wuwe::runtime_host::runtime_error& ex) {
    expect(ex.code() == wuwe::runtime_host::runtime_error_code::dependency_unavailable,
      "startup must not fall back to a provider default environment variable");
  }
}

void dependency_injection_supports_custom_providers() {
  auto document = valid_config_document();
  document["llm"].erase("apiKeyEnv");
  document["llm"]["provider"] = "OpenAI";
  const auto config = wuwe::runtime_host::runtime_config_from_json(document);
  const auto dependencies = dependencies_for(std::make_shared<fake_llm_client>());
  expect(!wuwe::runtime_host::diagnose_runtime_config(config).ready,
    "default preflight must still enforce the built-in provider's credential requirement");
  expect(wuwe::runtime_host::diagnose_runtime_config(config, dependencies).ready,
    "preflight diagnostics must recognize responsibility delegated to custom dependencies");
  wuwe::runtime_host::host host(dependencies);
  host.configure(config);
  host.start();
  expect(host.run("custom provider").status == wuwe::agent::runtime::agent_run_status::completed,
    "a custom injected client must be able to override a built-in provider's credential policy");
  expect(host.stop(), "custom-provider runtime should stop cleanly");
}

void dependency_injection_supports_runner_configuration() {
  auto client = std::make_shared<fake_llm_client>();
  auto dependencies = dependencies_for(client);
  bool runner_factory_called = false;
  dependencies.make_agent_runner = [&](wuwe::llm_client& configured_client) {
    runner_factory_called = true;
    return std::make_shared<wuwe::llm_agent_runner>(configured_client);
  };
  wuwe::runtime_host::host host(std::move(dependencies));
  host.configure(wuwe::runtime_host::runtime_config_from_json(valid_config_document()));
  host.start();
  expect(runner_factory_called, "Runtime Host must assemble the runner through its public factory");
  expect(host.stop(), "runner-configured runtime should stop cleanly");
}

void runtime_executes_real_runner_and_persists_run() {
  auto client = std::make_shared<fake_llm_client>();
  wuwe::runtime_host::host host(dependencies_for(client));
  host.configure(wuwe::runtime_host::runtime_config_from_json(valid_config_document()));
  host.start();
  expect(host.health().status == wuwe::runtime_host::health_status::healthy,
    "started runtime should be healthy");
  const auto result = host.run("hello runtime");
  expect(result.status == wuwe::agent::runtime::agent_run_status::completed,
    "successful model response should complete the durable run");
  expect(result.content == "completed: hello runtime", "runtime must return the model response");
  const auto stored = host.get_run(result.run_id);
  expect(stored && stored->revision >= 3, "durable run should contain lifecycle revisions");
  expect(stored->result.at("content") == result.content,
    "durable result should contain the model response");
  wuwe::agent::core::agent_execution_context duplicate_context;
  duplicate_context.run_id = result.run_id;
  try {
    (void)host.run("duplicate", duplicate_context);
    throw std::runtime_error("duplicate durable run id should have been rejected");
  }
  catch (const wuwe::runtime_host::runtime_error& ex) {
    expect(ex.code() == wuwe::runtime_host::runtime_error_code::run_conflict,
      "duplicate durable run ids must have a stable conflict classification");
  }
  expect(host.stop(), "runtime should stop gracefully");
  expect(host.stop(), "runtime stop should be idempotent");
}

void cancellation_propagates_to_the_model_client() {
  auto client = std::make_shared<fake_llm_client>();
  client->block_until_cancelled();
  wuwe::runtime_host::host host(dependencies_for(client));
  host.configure(wuwe::runtime_host::runtime_config_from_json(valid_config_document()));
  host.start();
  wuwe::agent::core::agent_execution_context context;
  context.run_id = "run-cancellation-test";
  auto run = std::async(std::launch::async, [&] { return host.run("wait", context); });
  client->wait_until_started();
  expect(host.cancel_run(context.run_id), "active run cancellation should be accepted");
  const auto result = run.get();
  expect(result.status == wuwe::agent::runtime::agent_run_status::cancelled,
    "cancellation should reach the durable run state");
  expect(host.stop(), "runtime should stop after cancellation");
}

void caller_cancellation_propagates_to_the_model_client() {
  auto client = std::make_shared<fake_llm_client>();
  client->block_until_cancelled();
  wuwe::runtime_host::host host(dependencies_for(client));
  host.configure(wuwe::runtime_host::runtime_config_from_json(valid_config_document()));
  host.start();
  std::stop_source caller_stop;
  wuwe::agent::core::agent_execution_context context;
  context.run_id = "run-caller-cancellation-test";
  context.stop_token = caller_stop.get_token();
  auto run = std::async(std::launch::async, [&] { return host.run("wait", context); });
  client->wait_until_started();
  caller_stop.request_stop();
  const auto result = run.get();
  expect(result.status == wuwe::agent::runtime::agent_run_status::cancelled,
    "the execution context stop token must reach the model client");
  expect(host.stop(), "runtime should stop after caller cancellation");
}

void destruction_waits_after_graceful_shutdown_timeout() {
  auto client = std::make_shared<fake_llm_client>();
  client->block_until_cancelled();
  client->delay_cancellation(50ms);
  auto host = std::make_unique<wuwe::runtime_host::host>(dependencies_for(client));
  host->configure(wuwe::runtime_host::runtime_config_from_json(valid_config_document()));
  host->start();
  wuwe::agent::core::agent_execution_context context;
  context.run_id = "run-destruction-timeout";
  auto run = std::async(std::launch::async, [&] { return host->run("wait", context); });
  client->wait_until_started();
  expect(!host->stop(1ms), "graceful shutdown should report an expired timeout");
  host.reset();
  const auto result = run.get();
  expect(result.status == wuwe::agent::runtime::agent_run_status::cancelled,
    "host destruction must keep execution state alive until cancellation completes");
}

void expired_deadline_is_persisted_as_timeout() {
  auto client = std::make_shared<fake_llm_client>();
  wuwe::runtime_host::host host(dependencies_for(client));
  host.configure(wuwe::runtime_host::runtime_config_from_json(valid_config_document()));
  host.start();
  wuwe::agent::core::agent_execution_context context;
  context.deadline = std::chrono::system_clock::now() - 1ms;
  const auto result = host.run("too late", context);
  expect(result.status == wuwe::agent::runtime::agent_run_status::timed_out,
    "expired execution deadline should be durable timeout state");
  expect(host.stop(), "runtime should stop after timeout");
}

void sqlite_store_recovers_completed_runs() {
#if WUWE_HAS_SQLITE
  const auto database =
    std::filesystem::temp_directory_path() / "wuwe-runtime-host-recovery-test.db";
  std::error_code ignored;
  std::filesystem::remove(database, ignored);
  std::filesystem::remove(database.string() + "-wal", ignored);
  std::filesystem::remove(database.string() + "-shm", ignored);
  auto document = valid_config_document();
  document["store"] = { { "kind", "sqlite" }, { "path", database.string() } };
  const auto config = wuwe::runtime_host::runtime_config_from_json(document);
  {
    wuwe::runtime_host::host host(dependencies_for(std::make_shared<fake_llm_client>()));
    host.configure(config);
    host.start();
    wuwe::agent::core::agent_execution_context context;
    context.run_id = "run-sqlite-recovery";
    const auto result = host.run("persist me", context);
    expect(result.status == wuwe::agent::runtime::agent_run_status::completed,
      "SQLite-backed run should complete");
    expect(host.stop(), "first SQLite runtime should stop");
  }
  {
    wuwe::runtime_host::host host(dependencies_for(std::make_shared<fake_llm_client>()));
    host.configure(config);
    host.start();
    const auto recovered = host.get_run("run-sqlite-recovery");
    expect(recovered && recovered->status == wuwe::agent::runtime::agent_run_status::completed,
      "new Runtime Host instance should recover the durable run");
    expect(host.stop(), "second SQLite runtime should stop");
  }
  std::filesystem::remove(database, ignored);
  std::filesystem::remove(database.string() + "-wal", ignored);
  std::filesystem::remove(database.string() + "-shm", ignored);
#endif
}

void sqlite_idempotency_replays_completed_mutations_across_restart() {
#if WUWE_HAS_SQLITE
  const auto database =
    std::filesystem::temp_directory_path() / "wuwe-runtime-idempotency-restart.db";
  std::error_code ignored;
  std::filesystem::remove(database, ignored);
  std::filesystem::remove(database.string() + "-wal", ignored);
  std::filesystem::remove(database.string() + "-shm", ignored);
  auto document = valid_config_document();
  document["store"] = { { "kind", "sqlite" }, { "path", database.string() } };
  const auto config = wuwe::runtime_host::runtime_config_from_json(document);
  auto client = std::make_shared<fake_llm_client>();
  const nlohmann::json request {
    { "protocolVersion", "2026-07-01" },
    { "requestId", "request-restart-replay" },
    { "idempotencyKey", "idempotency-restart-replay" },
    { "operation", "create_run" },
    { "body",
      {
        { "context", { { "runId", "run-restart-replay" } } },
        { "input", { { "prompt", "execute exactly once across host restart" } } },
      } },
  };
  nlohmann::json original;
  {
    wuwe::runtime_host::host host(dependencies_for(client));
    host.configure(config);
    host.start();
    original = host.dispatch_protocol_request(request);
    expect(original.at("ok").get<bool>(), "initial durable mutation should succeed");
    expect(host.stop(), "first durable host should stop cleanly");
  }
  expect(client->completion_count() == 1, "initial mutation should execute the model once");
  {
    wuwe::runtime_host::host host(dependencies_for(client));
    host.configure(config);
    host.start();
    const auto replay = host.dispatch_protocol_request(request);
    expect(replay.at("ok").get<bool>() && replay.at("body") == original.at("body"),
      "completed SQLite mutation must replay its original submission after restart");
    expect(client->completion_count() == 1,
      "cross-restart idempotency replay must not execute the model again");
    auto conflicting = request;
    conflicting["body"]["input"]["prompt"] = "different request";
    const auto conflict = host.dispatch_protocol_request(conflicting);
    expect(!conflict.at("ok").get<bool>() && conflict.at("error").at("code") == "conflict",
      "SQLite idempotency fingerprint conflicts must survive host restart");
    expect(host.stop(), "second durable host should stop cleanly");
  }
  std::filesystem::remove(database, ignored);
  std::filesystem::remove(database.string() + "-wal", ignored);
  std::filesystem::remove(database.string() + "-shm", ignored);
#endif
}

void sqlite_idempotency_preserves_unresolved_claims_across_restart() {
#if WUWE_HAS_SQLITE
  const auto database =
    std::filesystem::temp_directory_path() / "wuwe-runtime-idempotency-pending.db";
  std::error_code ignored;
  std::filesystem::remove(database, ignored);
  std::filesystem::remove(database.string() + "-wal", ignored);
  std::filesystem::remove(database.string() + "-shm", ignored);
  {
    auto repository = wuwe::runtime_host::detail::make_sqlite_idempotency_repository(database, 16);
    const auto claim = repository->claim("operation:key", "fingerprint-a");
    expect(claim.status == wuwe::runtime_host::detail::idempotency_claim_status::acquired,
      "first durable idempotency claimant should acquire the key");
  }
  {
    auto repository = wuwe::runtime_host::detail::make_sqlite_idempotency_repository(database, 16);
    const auto pending = repository->claim("operation:key", "fingerprint-a");
    expect(pending.status == wuwe::runtime_host::detail::idempotency_claim_status::pending,
      "an unfinished durable claim must remain unresolved after repository restart");
    const auto conflict = repository->claim("operation:key", "fingerprint-b");
    expect(conflict.status == wuwe::runtime_host::detail::idempotency_claim_status::conflict,
      "an unresolved claim must still protect its key from a different request");
    repository->erase("operation:key", "fingerprint-a");
    const auto reacquired = repository->claim("operation:key", "fingerprint-a");
    expect(reacquired.status == wuwe::runtime_host::detail::idempotency_claim_status::acquired,
      "explicit reconciliation must make an unresolved key reusable");
  }
  std::filesystem::remove(database, ignored);
  std::filesystem::remove(database.string() + "-wal", ignored);
  std::filesystem::remove(database.string() + "-shm", ignored);
#endif
}

void host_protocol_dispatches_to_the_runtime() {
  auto client = std::make_shared<fake_llm_client>();
  wuwe::runtime_host::host host(dependencies_for(client));
  host.configure(wuwe::runtime_host::runtime_config_from_json(valid_config_document()));
  host.start();
  const nlohmann::json request {
    { "protocolVersion", "2026-07-01" },
    { "requestId", "request-1" },
    { "idempotencyKey", "idempotency-1" },
    { "operation", "create_run" },
    { "metadata", nlohmann::json::object() },
    { "body",
      {
        { "context", { { "runId", "run-host-protocol" } } },
        { "input", { { "prompt", "from protocol" } } },
        { "metadata", nlohmann::json::object() },
      } },
  };
  const auto response = host.dispatch_protocol_request(request);
  expect(response.at("ok").get<bool>(), "Host Protocol create_run should succeed");
  expect(response.at("body").at("status") == "running" ||
           response.at("body").at("status") == "completed",
    "Host Protocol should expose the durable submission status");
  const auto replay = host.dispatch_protocol_request(request);
  expect(replay.at("ok").get<bool>() && replay.at("body") == response.at("body"),
    "replayed idempotent request should return the original submission");
  auto different_call_metadata = request;
  different_call_metadata["metadata"] = { { "principal", "different" } };
  const auto metadata_conflict = host.dispatch_protocol_request(different_call_metadata);
  expect(!metadata_conflict.at("ok").get<bool>() &&
           metadata_conflict.at("error").at("code") == "conflict",
    "idempotency fingerprints must include operation call metadata");
  auto conflicting = request;
  conflicting["body"]["input"]["prompt"] = "different input";
  const auto conflict = host.dispatch_protocol_request(conflicting);
  expect(!conflict.at("ok").get<bool>() && conflict.at("error").at("code") == "conflict",
    "reusing an idempotency key for different input must be rejected");

  auto stored = host.get_run("run-host-protocol");
  expect(stored.has_value(), "created Host Protocol run should be durable");
  const nlohmann::json cancel_with_operation_scoped_key {
    { "protocolVersion", "2026-07-01" },
    { "requestId", "request-cancel-operation-scope" },
    { "idempotencyKey", "idempotency-1" },
    { "operation", "cancel_run" },
    { "body",
      {
        { "runId", "run-host-protocol" },
        { "expectedRevision", stored->revision },
        { "reason", "operation-scoped idempotency test" },
      } },
  };
  const auto operation_scoped = host.dispatch_protocol_request(cancel_with_operation_scoped_key);
  expect(operation_scoped.at("ok").get<bool>(),
    "the same idempotency key must be reusable for a different mutation operation");
  expect(host.stop(), "runtime should stop after protocol dispatch");
}

void host_protocol_rejects_unsafe_or_ambiguous_run_metadata() {
  wuwe::runtime_host::host host(dependencies_for(std::make_shared<fake_llm_client>()));
  host.configure(wuwe::runtime_host::runtime_config_from_json(valid_config_document()));
  host.start();
  const nlohmann::json sensitive {
    { "protocolVersion", "2026-07-01" },
    { "requestId", "request-sensitive-metadata" },
    { "idempotencyKey", "idempotency-sensitive-metadata" },
    { "operation", "create_run" },
    { "body",
      {
        { "context", { { "runId", "run-sensitive-metadata" } } },
        { "input", { { "prompt", "must be rejected" } } },
        { "metadata", { { "api_key", "secret" } } },
      } },
  };
  const auto sensitive_response = host.dispatch_protocol_request(sensitive);
  expect(!sensitive_response.at("ok").get<bool>() &&
           sensitive_response.at("error").at("code") == "invalid_request",
    "run metadata must reject sensitive keys before durable persistence");

  auto sensitive_call = sensitive;
  sensitive_call["requestId"] = "request-sensitive-call-metadata";
  sensitive_call["idempotencyKey"] = "idempotency-sensitive-call-metadata";
  sensitive_call["metadata"] = { { "authorization", "Bearer secret" } };
  sensitive_call["body"]["context"]["runId"] = "run-sensitive-call-metadata";
  sensitive_call["body"]["metadata"] = nlohmann::json::object();
  const auto sensitive_call_response = host.dispatch_protocol_request(sensitive_call);
  expect(!sensitive_call_response.at("ok").get<bool>() &&
           sensitive_call_response.at("error").at("code") == "invalid_request",
    "call metadata must reject sensitive keys before entering the idempotency cache");

  auto ambiguous = sensitive;
  ambiguous["requestId"] = "request-ambiguous-metadata";
  ambiguous["idempotencyKey"] = "idempotency-ambiguous-metadata";
  ambiguous["body"]["context"]["runId"] = "run-ambiguous-metadata";
  ambiguous["body"]["context"]["metadata"] = { { "region", "west" } };
  ambiguous["body"]["metadata"] = { { "region", "east" } };
  const auto ambiguous_response = host.dispatch_protocol_request(ambiguous);
  expect(!ambiguous_response.at("ok").get<bool>() &&
           ambiguous_response.at("error").at("code") == "invalid_request",
    "conflicting metadata sources must not be merged with implicit precedence");

  const nlohmann::json sensitive_approval {
    { "protocolVersion", "2026-07-01" },
    { "requestId", "request-sensitive-approval-metadata" },
    { "idempotencyKey", "idempotency-sensitive-approval-metadata" },
    { "operation", "resolve_approval" },
    { "body",
      {
        { "runId", "missing-approval-run" },
        { "expectedRevision", 1 },
        { "approvalId", "missing-approval" },
        { "resolution", "approved" },
        { "metadata", { { "access_token", "secret" } } },
      } },
  };
  const auto approval_response = host.dispatch_protocol_request(sensitive_approval);
  expect(!approval_response.at("ok").get<bool>() &&
           approval_response.at("error").at("code") == "invalid_request",
    "approval metadata must reject sensitive keys before durable persistence");
  expect(host.stop(), "runtime should stop after metadata validation tests");
}

void resume_capacity_is_reported_as_retryable_unavailable() {
  auto client = std::make_shared<fake_llm_client>();
  client->block_until_cancelled();
  auto store = std::make_shared<wuwe::agent::runtime::in_memory_agent_run_store>();
  wuwe::agent::runtime::agent_run_runtime setup(store);
  wuwe::agent::core::agent_execution_context resume_context;
  resume_context.run_id = "run-resume-capacity";
  auto record = setup.start(resume_context);
  auto write = setup.transition(
    record.id, record.revision, wuwe::agent::runtime::agent_run_status::running, "run_started");
  expect(static_cast<bool>(write), "resume fixture should transition to running");
  wuwe::agent::runtime::agent_run_suspension suspension {
    .approval_id = "approval-resume-capacity",
    .continuation_token = "continuation-resume-capacity",
    .tool_call_id = "tool-call-resume-capacity",
    .tool_name = "test_tool",
  };
  write = setup.suspend_for_approval(record.id, write.revision, suspension);
  expect(static_cast<bool>(write), "resume fixture should suspend for approval");
  write = setup.resolve_approval(record.id,
    write.revision,
    suspension.continuation_token,
    wuwe::agent::runtime::approval_resolution::approved);
  expect(static_cast<bool>(write), "resume fixture approval should resolve");

  auto document = valid_config_document();
  document["runtime"]["maxConcurrentRuns"] = 1;
  auto dependencies = dependencies_for(client);
  dependencies.make_run_store = [store](const auto&) { return store; };
  wuwe::runtime_host::host host(std::move(dependencies));
  host.configure(wuwe::runtime_host::runtime_config_from_json(document));
  host.start();
  wuwe::agent::core::agent_execution_context active_context;
  active_context.run_id = "run-resume-capacity-holder";
  auto active = std::async(std::launch::async, [&] { return host.run("wait", active_context); });
  client->wait_until_started();
  const nlohmann::json resume {
    { "protocolVersion", "2026-07-01" },
    { "requestId", "request-resume-over-capacity" },
    { "idempotencyKey", "idempotency-resume-over-capacity" },
    { "operation", "resume_run" },
    { "body",
      {
        { "runId", record.id },
        { "expectedRevision", write.revision },
        { "approvalId", suspension.approval_id },
        { "input", nlohmann::json::object() },
      } },
  };
  const auto rejected = host.dispatch_protocol_request(resume);
  expect(!rejected.at("ok").get<bool>() && rejected.at("error").at("code") == "unavailable" &&
           rejected.at("error").at("retryable").get<bool>(),
    "resume capacity exhaustion must be retryable unavailable rather than internal");
  expect(host.cancel_run(active_context.run_id), "resume capacity-holder cancellation should work");
  expect(active.get().status == wuwe::agent::runtime::agent_run_status::cancelled,
    "resume capacity-holder should cancel cleanly");
  expect(host.stop(), "runtime should stop after resume capacity validation");
}

void host_protocol_submission_is_cancellable() {
  auto client = std::make_shared<fake_llm_client>();
  client->block_until_cancelled();
  wuwe::runtime_host::host host(dependencies_for(client));
  host.configure(wuwe::runtime_host::runtime_config_from_json(valid_config_document()));
  host.start();
  const nlohmann::json create {
    { "protocolVersion", "2026-07-01" },
    { "requestId", "request-create-blocking" },
    { "idempotencyKey", "idempotency-create-blocking" },
    { "operation", "create_run" },
    { "body",
      {
        { "context", { { "runId", "run-host-cancellation" } } },
        { "input", { { "prompt", "wait" } } },
      } },
  };
  const auto created = host.dispatch_protocol_request(create);
  expect(created.at("ok").get<bool>() && created.at("body").at("status") == "running",
    "create_run must return while the model call is still active");
  auto duplicate = create;
  duplicate["requestId"] = "request-create-duplicate";
  duplicate["idempotencyKey"] = "idempotency-create-duplicate";
  const auto duplicate_response = host.dispatch_protocol_request(duplicate);
  expect(!duplicate_response.at("ok").get<bool>() &&
           duplicate_response.at("error").at("code") == "conflict",
    "an already active run id must report conflict rather than capacity or invalid_request");
  const auto revision = created.at("body").at("revision").get<std::uint64_t>();
  const nlohmann::json cancel {
    { "protocolVersion", "2026-07-01" },
    { "requestId", "request-cancel-blocking" },
    { "idempotencyKey", "idempotency-cancel-blocking" },
    { "operation", "cancel_run" },
    { "body",
      {
        { "runId", "run-host-cancellation" },
        { "expectedRevision", revision },
        { "reason", "test cancellation" },
      } },
  };
  const auto cancelled = host.dispatch_protocol_request(cancel);
  expect(cancelled.at("ok").get<bool>(), "cancel_run should accept an active Host Protocol run");
  for (int attempt = 0; attempt < 200; ++attempt) {
    const auto stored = host.get_run("run-host-cancellation");
    if (stored && stored->status == wuwe::agent::runtime::agent_run_status::cancelled)
      break;
    std::this_thread::sleep_for(2ms);
  }
  const auto stored = host.get_run("run-host-cancellation");
  expect(stored && stored->status == wuwe::agent::runtime::agent_run_status::cancelled,
    "Host Protocol cancellation should propagate to durable state");
  expect(host.stop(), "runtime should stop after Host Protocol cancellation");
}

void capacity_is_reserved_before_background_thread_creation() {
  auto client = std::make_shared<fake_llm_client>();
  client->block_until_cancelled();
  auto document = valid_config_document();
  document["runtime"]["maxConcurrentRuns"] = 1;
  wuwe::runtime_host::host host(dependencies_for(client));
  host.configure(wuwe::runtime_host::runtime_config_from_json(document));
  host.start();
  wuwe::agent::core::agent_execution_context context;
  context.run_id = "run-capacity-holder";
  auto active = std::async(std::launch::async, [&] { return host.run("wait", context); });
  client->wait_until_started();
  const nlohmann::json request {
    { "protocolVersion", "2026-07-01" },
    { "requestId", "request-over-capacity" },
    { "idempotencyKey", "idempotency-over-capacity" },
    { "operation", "create_run" },
    { "body",
      {
        { "context", { { "runId", "run-over-capacity" } } },
        { "input", { { "prompt", "must not start" } } },
      } },
  };
  const auto rejected = host.dispatch_protocol_request(request);
  expect(!rejected.at("ok").get<bool>() && rejected.at("error").at("code") == "unavailable" &&
           rejected.at("error").at("retryable").get<bool>(),
    "capacity must be rejected synchronously before a background worker is launched");
  expect(host.cancel_run(context.run_id), "capacity-holder cancellation should be accepted");
  expect(active.get().status == wuwe::agent::runtime::agent_run_status::cancelled,
    "capacity-holder run should cancel cleanly");
  client->stop_blocking();
  const auto retried = host.dispatch_protocol_request(request);
  expect(retried.at("ok").get<bool>(),
    "a retryable capacity failure must not poison later retries with the same idempotency key");
  expect(host.stop(), "runtime should stop after the capacity test");
}

void sqlite_retryable_failures_do_not_poison_restart_retries() {
#if WUWE_HAS_SQLITE
  const auto database =
    std::filesystem::temp_directory_path() / "wuwe-runtime-idempotency-retry.db";
  std::error_code ignored;
  std::filesystem::remove(database, ignored);
  std::filesystem::remove(database.string() + "-wal", ignored);
  std::filesystem::remove(database.string() + "-shm", ignored);
  auto document = valid_config_document();
  document["runtime"]["maxConcurrentRuns"] = 1;
  document["store"] = { { "kind", "sqlite" }, { "path", database.string() } };
  const auto config = wuwe::runtime_host::runtime_config_from_json(document);
  auto client = std::make_shared<fake_llm_client>();
  client->block_until_cancelled();
  const nlohmann::json request {
    { "protocolVersion", "2026-07-01" },
    { "requestId", "request-sqlite-retry" },
    { "idempotencyKey", "idempotency-sqlite-retry" },
    { "operation", "create_run" },
    { "body",
      {
        { "context", { { "runId", "run-sqlite-retry" } } },
        { "input", { { "prompt", "retry after restart" } } },
      } },
  };
  {
    wuwe::runtime_host::host host(dependencies_for(client));
    host.configure(config);
    host.start();
    wuwe::agent::core::agent_execution_context active_context;
    active_context.run_id = "run-sqlite-retry-holder";
    auto active = std::async(std::launch::async, [&] { return host.run("wait", active_context); });
    client->wait_until_started();
    const auto rejected = host.dispatch_protocol_request(request);
    expect(!rejected.at("ok").get<bool>() && rejected.at("error").at("code") == "unavailable" &&
             rejected.at("error").at("retryable").get<bool>(),
      "SQLite-backed capacity failure should be retryable");
    expect(host.cancel_run(active_context.run_id), "SQLite retry holder should cancel");
    expect(active.get().status == wuwe::agent::runtime::agent_run_status::cancelled,
      "SQLite retry holder should reach cancelled state");
    expect(host.stop(), "first SQLite retry host should stop cleanly");
  }
  client->stop_blocking();
  {
    wuwe::runtime_host::host host(dependencies_for(client));
    host.configure(config);
    host.start();
    const auto retried = host.dispatch_protocol_request(request);
    expect(retried.at("ok").get<bool>(),
      "retryable SQLite availability failures must not survive host restart");
    expect(host.stop(), "second SQLite retry host should stop cleanly");
  }
  std::filesystem::remove(database, ignored);
  std::filesystem::remove(database.string() + "-wal", ignored);
  std::filesystem::remove(database.string() + "-shm", ignored);
#endif
}

} // namespace

int main() {
  try {
    config_validation_is_strict_and_side_effect_free();
    plugin_config_paths_are_resolved_from_the_config_file();
    lifecycle_contract_is_explicit();
    plugin_manifest_rejects_native_loading();
    plugin_packages_are_admitted_and_registered_by_verified_identity();
    plugin_package_admission_rejects_missing_or_ambiguous_files();
    plugin_package_admission_rejects_link_aliases();
    plugin_process_execution_is_explicit_and_capability_gated();
    plugin_process_environment_is_allowlisted_and_packages_are_revalidated();
    plugin_activation_environment_is_scoped_per_package();
    plugin_runtime_coordinates_catalog_and_process_lifecycle();
    plugin_tools_are_explicitly_bound_into_the_tool_contract();
    runtime_host_activates_only_configured_plugin_tools();
    runtime_host_plugin_activation_fails_closed();
    provider_specific_endpoint_requirements_are_preflighted();
    configured_credential_reference_is_authoritative();
    dependency_injection_supports_custom_providers();
    dependency_injection_supports_runner_configuration();
    runtime_executes_real_runner_and_persists_run();
    cancellation_propagates_to_the_model_client();
    caller_cancellation_propagates_to_the_model_client();
    destruction_waits_after_graceful_shutdown_timeout();
    expired_deadline_is_persisted_as_timeout();
    sqlite_store_recovers_completed_runs();
    sqlite_idempotency_replays_completed_mutations_across_restart();
    sqlite_idempotency_preserves_unresolved_claims_across_restart();
    host_protocol_dispatches_to_the_runtime();
    host_protocol_rejects_unsafe_or_ambiguous_run_metadata();
    resume_capacity_is_reported_as_retryable_unavailable();
    host_protocol_submission_is_cancellable();
    capacity_is_reserved_before_background_thread_creation();
    sqlite_retryable_failures_do_not_poison_restart_retries();
    std::cout << "wuwe runtime host tests passed\n";
    return 0;
  }
  catch (const std::exception& ex) {
    std::cerr << "wuwe runtime host tests failed: " << ex.what() << '\n';
    return 1;
  }
}
