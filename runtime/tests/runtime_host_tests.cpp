#include <chrono>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include <wuwe/agent/llm/llm_agent_runner.h>
#include <wuwe/agent/runtime/run_runtime.hpp>
#include <wuwe/runtime_host.hpp>

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

class fake_llm_client final : public wuwe::llm_client {
public:
  wuwe::llm_response complete(const wuwe::llm_request& request) override {
    return complete(request, {});
  }

  wuwe::llm_response complete(
    const wuwe::llm_request& request, std::stop_token stop_token) override {
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

private:
  bool block_ { false };
  std::promise<void> started_;
  std::future<void> started_future_;
  std::chrono::milliseconds cancellation_delay_ { 0 };
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
  const nlohmann::json process_manifest {
    { "schemaVersion", 1 },
    { "id", "com.example.tools" },
    { "version", "1.2.3" },
    { "runtimeApiVersion", "1.0" },
    { "entrypoint", { { "kind", "process" }, { "path", "bin/tools" } } },
    { "capabilities", { "tools" } },
    { "permissions", { { "networkHosts", { "api.example.com" } } } },
  };
  expect(wuwe::runtime_host::validate_plugin_manifest(process_manifest).valid(),
    "isolated process plugin manifest should be valid");
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
  auto typed = wuwe::runtime_host::plugin_manifest_from_json(process_manifest);
  typed.entrypoint_kind = static_cast<wuwe::runtime_host::plugin_entrypoint_kind>(999);
  expect(
    !wuwe::runtime_host::validate_plugin_manifest(
       wuwe::runtime_host::plugin_manifest_to_json(typed))
       .valid(),
    "plugin serialization must not normalize an unknown entrypoint kind");
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

} // namespace

int main() {
  try {
    config_validation_is_strict_and_side_effect_free();
    lifecycle_contract_is_explicit();
    plugin_manifest_rejects_native_loading();
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
    host_protocol_dispatches_to_the_runtime();
    host_protocol_rejects_unsafe_or_ambiguous_run_metadata();
    resume_capacity_is_reported_as_retryable_unavailable();
    host_protocol_submission_is_cancellable();
    capacity_is_reserved_before_background_thread_creation();
    std::cout << "wuwe runtime host tests passed\n";
    return 0;
  }
  catch (const std::exception& ex) {
    std::cerr << "wuwe runtime host tests failed: " << ex.what() << '\n';
    return 1;
  }
}
