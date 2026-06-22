#include <cassert>
#include <chrono>
#include <stdexcept>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include <wuwe/agent/approval/approval_service.hpp>
#include <wuwe/agent/audit/audit_sink.hpp>
#include <wuwe/agent/execution/execution.hpp>
#include <wuwe/agent/sandbox/sandbox.hpp>

namespace execution = wuwe::agent::execution;
namespace sandbox = wuwe::agent::sandbox;

std::string escape_python_string(std::string value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (const auto ch : value) {
    if (ch == '\\' || ch == '\'') {
      escaped.push_back('\\');
    }
    escaped.push_back(ch);
  }
  return escaped;
}

class recording_backend final : public execution::execution_backend {
public:
  sandbox::sandbox_backend_info info() const override {
    return {
      .name = "recording",
      .isolation = sandbox::isolation_level::controlled_process,
      .features = {
        sandbox::sandbox_feature::environment_allowlist,
        sandbox::sandbox_feature::timeout,
        sandbox::sandbox_feature::stdout_capture,
        sandbox::sandbox_feature::stderr_capture,
      },
    };
  }

  execution::execution_result run(
    const execution::execution_request& request,
    std::stop_token) override {
    ++calls;
    last_request = request;
    execution::execution_result result {
      .exit_code = 0,
      .termination_reason = execution::execution_termination_reason::exited,
      .stdout_text = "ok:" + request.code,
    };
    result.metadata["backend"] = "recording";
    return result;
  }

  int calls { 0 };
  execution::execution_request last_request;
};

class throwing_backend final : public execution::execution_backend {
public:
  sandbox::sandbox_backend_info info() const override {
    return {
      .name = "throwing",
      .isolation = sandbox::isolation_level::controlled_process,
    };
  }

  execution::execution_result run(
    const execution::execution_request&,
    std::stop_token) override {
    throw std::runtime_error("backend failed");
  }
};

void test_policy_denies_disallowed_language() {
  auto backend = std::make_unique<recording_backend>();
  auto* backend_ptr = backend.get();
  execution::execution_policy policy;
  policy.allowed_languages.clear();

  execution::execution_runtime runtime(std::move(backend), policy);

  execution::execution_request request;
  request.code = "print(1)";
  const auto result = runtime.run(request);

  assert(result.termination_reason == execution::execution_termination_reason::policy_denied);
  assert(!result.error_message.empty());
  assert(backend_ptr->calls == 0);
}

void test_runtime_clamps_limits_and_uses_env_allowlist() {
  auto backend = std::make_unique<recording_backend>();
  auto* backend_ptr = backend.get();

  execution::execution_policy policy;
  policy.max_limits.timeout = std::chrono::milliseconds(100);
  policy.max_limits.max_stdout_bytes = 10;
  policy.max_limits.max_stderr_bytes = 20;
  policy.allowed_env = { { "SAFE_ENV", "1" } };

  execution::execution_runtime runtime(std::move(backend), policy);

  execution::execution_request request;
  request.code = "print(1)";
  request.limits.timeout = std::chrono::milliseconds(5000);
  request.limits.max_stdout_bytes = 1000;
  request.limits.max_stderr_bytes = 1000;
  request.env = { { "UNSAFE_ENV", "secret" } };

  const auto result = runtime.run(request);

  assert(result.termination_reason == execution::execution_termination_reason::exited);
  assert(backend_ptr->calls == 1);
  assert(backend_ptr->last_request.limits.timeout == std::chrono::milliseconds(100));
  assert(backend_ptr->last_request.limits.max_stdout_bytes == 10);
  assert(backend_ptr->last_request.limits.max_stderr_bytes == 20);
  assert(backend_ptr->last_request.env.size() == 1);
  assert(backend_ptr->last_request.env.at("SAFE_ENV") == "1");
  assert(!backend_ptr->last_request.env.contains("UNSAFE_ENV"));
}

void test_runtime_clamps_resource_limits_and_audits() {
  auto backend = std::make_unique<recording_backend>();
  auto* backend_ptr = backend.get();
  wuwe::agent::audit::in_memory_audit_sink audit;

  execution::execution_policy policy;
  policy.max_limits.max_process_count = 2;
  policy.max_limits.max_memory_bytes = 1024;
  policy.max_limits.max_cpu_time = std::chrono::milliseconds(50);

  execution::execution_runtime runtime(std::move(backend), policy, &audit);

  execution::execution_request request;
  request.code = "print(1)";
  request.limits.max_process_count = 10;
  request.limits.max_memory_bytes = 2048;
  request.limits.max_cpu_time = std::chrono::milliseconds(500);
  const auto result = runtime.run(request);

  assert(result.termination_reason == execution::execution_termination_reason::exited);
  assert(backend_ptr->calls == 1);
  assert(backend_ptr->last_request.limits.max_process_count == 2);
  assert(backend_ptr->last_request.limits.max_memory_bytes == 1024);
  assert(backend_ptr->last_request.limits.max_cpu_time == std::chrono::milliseconds(50));
  assert(result.metadata.at("max_process_count_clamped") == "true");
  assert(result.metadata.at("max_memory_bytes_clamped") == "true");
  assert(result.metadata.at("max_cpu_time_clamped") == "true");
  const auto events = audit.events();
  assert(events.front().attributes.at("max_process_count_clamped") == "true");
  assert(events.front().attributes.at("max_memory_bytes_clamped") == "true");
  assert(events.front().attributes.at("max_cpu_time_clamped") == "true");
}

void test_policy_denies_invalid_allowed_environment_before_backend() {
  auto backend = std::make_unique<recording_backend>();
  auto* backend_ptr = backend.get();

  execution::execution_policy policy;
  policy.allowed_env = { { "BAD=NAME", "1" } };

  execution::execution_runtime runtime(std::move(backend), policy);

  execution::execution_request request;
  request.code = "print(1)";
  const auto result = runtime.run(request);

  assert(result.termination_reason == execution::execution_termination_reason::policy_denied);
  assert(result.error_message.find("invalid execution environment") != std::string::npos);
  assert(backend_ptr->calls == 0);
}

void test_approval_required_without_service_denies_before_backend() {
  auto backend = std::make_unique<recording_backend>();
  auto* backend_ptr = backend.get();
  wuwe::agent::audit::in_memory_audit_sink audit;

  execution::execution_policy policy;
  policy.allow_network = true;
  policy.require_approval_for_network = true;

  execution::execution_runtime runtime(std::move(backend), policy, &audit);

  execution::execution_request request;
  request.code = "print(1)";
  const auto result = runtime.run(request);

  assert(result.termination_reason == execution::execution_termination_reason::approval_denied);
  assert(backend_ptr->calls == 0);
  assert(audit.events().size() == 2);
}

void test_approval_allows_backend_and_audit_records_completion() {
  auto backend = std::make_unique<recording_backend>();
  auto* backend_ptr = backend.get();
  wuwe::agent::audit::in_memory_audit_sink audit;
  wuwe::agent::approval::allow_all_approval_service approvals;

  execution::execution_policy policy;
  policy.allow_file_write = true;
  policy.require_approval_for_file_write = true;

  execution::execution_runtime runtime(
    std::move(backend),
    policy,
    &audit,
    &approvals);

  execution::execution_request request;
  request.code = "print(1)";
  const auto result = runtime.run(request);

  assert(result.termination_reason == execution::execution_termination_reason::exited);
  assert(backend_ptr->calls == 1);
  const auto events = audit.events();
  assert(events.size() == 4);
  assert(events.back().attributes.at("termination_reason") == "exited");
}

void test_tool_provider_exposes_narrow_schema_and_invokes_runtime() {
  auto backend = std::make_unique<recording_backend>();
  auto* backend_ptr = backend.get();

  execution::execution_policy policy;
  policy.max_limits.timeout = std::chrono::milliseconds(250);
  policy.max_limits.max_code_bytes = 64;
  policy.max_limits.max_stdin_bytes = 128;

  execution::execution_runtime runtime(std::move(backend), policy);
  execution::execution_tool_provider provider(runtime);

  const auto tools = provider.tools();
  assert(tools.size() == 1);
  assert(tools[0].name == "run_python_snippet");
  assert(tools[0].parameters_json_schema.find("allow_network") == std::string::npos);
  assert(tools[0].parameters_json_schema.find("allow_file_write") == std::string::npos);
  assert(tools[0].parameters_json_schema.find("env") == std::string::npos);
  const auto schema = nlohmann::json::parse(tools[0].parameters_json_schema);
  assert(schema.at("additionalProperties") == false);
  assert(schema.at("properties").at("code").at("maxLength") == 64);
  assert(schema.at("properties").at("stdin_text").at("maxLength") == 128);
  assert(schema.at("properties").at("timeout_ms").at("maximum") == 250);

  nlohmann::json args {
    { "code", "print(42)" },
  };
  const auto result = provider.invoke("run_python_snippet", args.dump());

  assert(!result.error_code);
  assert(backend_ptr->calls == 1);
  assert(backend_ptr->last_request.limits.timeout == std::chrono::milliseconds(250));
  assert(backend_ptr->last_request.metadata.at("tool_name") == "run_python_snippet");

  const auto content = nlohmann::json::parse(result.content);
  assert(content.at("termination_reason") == "exited");
  assert(content.at("stdout_text") == "ok:print(42)");
}

void test_tool_provider_rejects_arguments_over_limit_before_parse() {
  auto backend = std::make_unique<recording_backend>();
  auto* backend_ptr = backend.get();
  wuwe::agent::audit::in_memory_audit_sink audit;

  execution::execution_policy policy;
  execution::execution_runtime runtime(std::move(backend), policy, &audit);
  execution::execution_tool_provider provider(
    runtime,
    { .max_arguments_bytes = 4 });

  const auto result = provider.invoke("run_python_snippet", "not json at all");

  assert(result.error_code);
  assert(backend_ptr->calls == 0);
  const auto content = nlohmann::json::parse(result.content);
  assert(content.at("termination_reason") == "policy_denied");
  assert(content.at("metadata").at("denial_kind") == "arguments_limit");
  assert(content.at("metadata").at("max_arguments_bytes") == "4");
  const auto events = audit.events();
  assert(events.size() == 1);
  assert(events[0].name == "arguments_limit");
}

void test_tool_provider_rejects_unknown_arguments_and_audits() {
  auto backend = std::make_unique<recording_backend>();
  auto* backend_ptr = backend.get();
  wuwe::agent::audit::in_memory_audit_sink audit;

  execution::execution_policy policy;
  execution::execution_runtime runtime(std::move(backend), policy, &audit);
  execution::execution_tool_provider provider(runtime);

  nlohmann::json args {
    { "code", "print(1)" },
    { "allow_network", true },
  };
  const auto result = provider.invoke("run_python_snippet", args.dump());

  assert(result.error_code);
  assert(backend_ptr->calls == 0);
  const auto content = nlohmann::json::parse(result.content);
  assert(content.at("termination_reason") == "policy_denied");
  assert(content.at("metadata").at("denial_kind") == "schema_invalid");
  assert(content.at("metadata").at("parse_error").get<std::string>().find(
           "unexpected field") != std::string::npos);
  const auto events = audit.events();
  assert(events.size() == 1);
  assert(events[0].name == "schema_invalid");
}

void test_tool_provider_rejects_timeout_over_schema_limit() {
  auto backend = std::make_unique<recording_backend>();
  auto* backend_ptr = backend.get();
  wuwe::agent::audit::in_memory_audit_sink audit;

  execution::execution_policy policy;
  policy.max_limits.timeout = std::chrono::milliseconds(100);
  execution::execution_runtime runtime(std::move(backend), policy, &audit);
  execution::execution_tool_provider provider(runtime);

  nlohmann::json args {
    { "code", "print(1)" },
    { "timeout_ms", 1000 },
  };
  const auto result = provider.invoke("run_python_snippet", args.dump());

  assert(result.error_code);
  assert(backend_ptr->calls == 0);
  const auto content = nlohmann::json::parse(result.content);
  assert(content.at("termination_reason") == "policy_denied");
  assert(content.at("metadata").at("denial_kind") == "timeout_limit");
  assert(content.at("metadata").at("timeout_ms") == "1000");
  assert(content.at("metadata").at("max_timeout_ms") == "100");
  const auto events = audit.events();
  assert(events.size() == 1);
  assert(events[0].name == "timeout_limit");
}

void test_runtime_audit_records_clamped_limits() {
  auto backend = std::make_unique<recording_backend>();
  wuwe::agent::audit::in_memory_audit_sink audit;

  execution::execution_policy policy;
  policy.max_limits.timeout = std::chrono::milliseconds(100);
  execution::execution_runtime runtime(std::move(backend), policy, &audit);

  execution::execution_request request;
  request.code = "print(1)";
  request.limits.timeout = std::chrono::milliseconds(1000);
  const auto result = runtime.run(request);

  assert(result.termination_reason == execution::execution_termination_reason::exited);
  assert(result.metadata.at("timeout_clamped") == "true");
  assert(result.metadata.at("requested_timeout_ms") == "1000");
  const auto events = audit.events();
  assert(events.front().attributes.at("timeout_clamped") == "true");
}

void test_default_backend_registry_exposes_controlled_process() {
  auto registry = execution::make_default_execution_backend_registry();
  const auto backends = registry.backends();
  assert(backends.size() >= 4);
  assert(backends.at(0).name == "controlled_process");
  assert(backends.at(0).available);
  assert(backends.at(0).enforcement.process_tree_cleanup ==
         sandbox::enforcement_level::enforced);
  assert(backends.at(0).enforcement.filesystem_read_deny ==
         sandbox::enforcement_level::not_enforced);
  assert(backends.at(1).name == "restricted_process");
  assert(!backends.at(1).available);
  assert(backends.at(1).enforcement.filesystem_read_deny ==
         sandbox::enforcement_level::planned);
  assert(backends.at(2).name == "container");
  assert(!backends.at(2).available);
  assert(backends.at(3).name == "wasm");
  assert(!backends.at(3).available);
  auto backend = registry.create("controlled_process");
  assert(backend != nullptr);
  assert(registry.create("missing") == nullptr);
}

void test_backend_registry_selects_only_available_enforced_backends() {
  auto registry = execution::make_default_execution_backend_registry();

  execution::execution_backend_requirements controlled_requirements;
  controlled_requirements.require_timeout = true;
  controlled_requirements.require_process_tree_cleanup = true;
  const auto controlled_name = registry.select_backend_name(controlled_requirements);
  assert(controlled_name.has_value());
  assert(*controlled_name == "controlled_process");
  assert(registry.create_best(controlled_requirements) != nullptr);

  execution::execution_backend_requirements strong_requirements;
  strong_requirements.require_filesystem_read_deny = true;
  strong_requirements.require_filesystem_write_deny = true;
  strong_requirements.require_network_deny = true;
  const auto strong_name = registry.select_backend_name(strong_requirements);
  assert(!strong_name.has_value());
  assert(registry.create_best(strong_requirements) == nullptr);

  const auto restricted = registry.describe("restricted_process");
  assert(restricted.has_value());
  assert(!restricted->available);
  assert(!restricted->unavailable_reason.empty());
  assert(registry.describe("missing") == std::nullopt);
}

void test_planned_backend_descriptors_are_not_executable() {
  auto registry = execution::make_default_execution_backend_registry();
  const auto restricted = registry.describe("restricted_process");
  assert(restricted.has_value());
  assert(!restricted->available);
  assert(restricted->isolation == sandbox::isolation_level::restricted_process);
  assert(restricted->enforcement.filesystem_read_deny ==
         sandbox::enforcement_level::planned);
  assert(!restricted->unavailable_reason.empty());
  assert(registry.create("restricted_process") == nullptr);
  assert(registry.create("container") == nullptr);
  assert(registry.create("wasm") == nullptr);
}

void test_controlled_process_contract_reflects_job_object_config() {
  execution::controlled_process_backend backend({
    .use_job_object = false,
  });
  const auto info = backend.info();

  assert(info.enforcement.process_tree_cleanup ==
         sandbox::enforcement_level::not_enforced);
  assert(info.enforcement.process_count_limit ==
         sandbox::enforcement_level::not_enforced);
  assert(info.enforcement.cpu_time_limit ==
         sandbox::enforcement_level::not_enforced);
  assert(info.enforcement.memory_limit ==
         sandbox::enforcement_level::not_enforced);
  assert(info.enforcement.filesystem_read_deny ==
         sandbox::enforcement_level::not_enforced);
  assert(info.enforcement.network_deny ==
         sandbox::enforcement_level::not_enforced);
}

void test_path_policy_rejects_prefix_trap() {
  const auto base = std::filesystem::temp_directory_path() / "wuwe-path-base";
  const auto sibling = std::filesystem::temp_directory_path() / "wuwe-path-base-other";
  const auto allowed = base / "child.txt";
  const auto rejected = sibling / "child.txt";

  const auto allowed_result = execution::evaluate_path_boundary(allowed, { base });
  const auto rejected_result = execution::evaluate_path_boundary(rejected, { base });

  assert(allowed_result.allowed);
  assert(!rejected_result.allowed);
}

void test_path_policy_handles_parent_traversal() {
  const auto base = std::filesystem::temp_directory_path() / "wuwe-path-parent";
  const auto allowed = base / "nested" / ".." / "child.txt";
  const auto rejected = base / ".." / "wuwe-path-parent-other" / "child.txt";

  const auto allowed_result = execution::evaluate_path_boundary(allowed, { base });
  const auto rejected_result = execution::evaluate_path_boundary(rejected, { base });

  assert(allowed_result.allowed);
  assert(!rejected_result.allowed);
}

void test_runtime_normalizes_backend_exceptions() {
  auto backend = std::make_unique<throwing_backend>();
  wuwe::agent::audit::in_memory_audit_sink audit;
  execution::execution_policy policy;
  execution::execution_runtime runtime(std::move(backend), policy, &audit);

  execution::execution_request request;
  request.code = "print(1)";
  const auto result = runtime.run(request);

  assert(result.termination_reason == execution::execution_termination_reason::backend_error);
  assert(result.error_message == "backend failed");
  assert(audit.events().back().attributes.at("termination_reason") == "backend_error");
}

void test_policy_denies_code_over_input_limit_before_backend() {
  auto backend = std::make_unique<recording_backend>();
  auto* backend_ptr = backend.get();
  wuwe::agent::audit::in_memory_audit_sink audit;

  execution::execution_policy policy;
  policy.max_limits.max_code_bytes = 4;
  policy.max_limits.max_stdin_bytes = 100;
  policy.max_limits.max_total_input_bytes = 100;
  execution::execution_runtime runtime(std::move(backend), policy, &audit);

  execution::execution_request request;
  request.code = "12345";
  const auto result = runtime.run(request);

  assert(result.termination_reason == execution::execution_termination_reason::policy_denied);
  assert(result.error_message.find("code is too large") != std::string::npos);
  assert(result.metadata.at("code_bytes") == "5");
  assert(result.metadata.at("max_code_bytes") == "4");
  assert(backend_ptr->calls == 0);
  const auto events = audit.events();
  assert(events.size() == 1);
  assert(events[0].name == "input_limit");
  assert(events[0].outcome == wuwe::agent::audit::audit_event_outcome::denied);
  assert(events[0].attributes.at("code_bytes") == "5");
}

void test_policy_denies_stdin_over_input_limit_before_backend() {
  auto backend = std::make_unique<recording_backend>();
  auto* backend_ptr = backend.get();

  execution::execution_policy policy;
  policy.max_limits.max_code_bytes = 100;
  policy.max_limits.max_stdin_bytes = 3;
  policy.max_limits.max_total_input_bytes = 100;
  execution::execution_runtime runtime(std::move(backend), policy);

  execution::execution_request request;
  request.code = "1";
  request.stdin_text = "1234";
  const auto result = runtime.run(request);

  assert(result.termination_reason == execution::execution_termination_reason::policy_denied);
  assert(result.error_message.find("stdin_text is too large") != std::string::npos);
  assert(result.metadata.at("stdin_bytes") == "4");
  assert(result.metadata.at("max_stdin_bytes") == "3");
  assert(backend_ptr->calls == 0);
}

void test_policy_denies_total_input_over_limit_before_backend() {
  auto backend = std::make_unique<recording_backend>();
  auto* backend_ptr = backend.get();

  execution::execution_policy policy;
  policy.max_limits.max_code_bytes = 10;
  policy.max_limits.max_stdin_bytes = 10;
  policy.max_limits.max_total_input_bytes = 7;
  execution::execution_runtime runtime(std::move(backend), policy);

  execution::execution_request request;
  request.code = "1234";
  request.stdin_text = "1234";
  const auto result = runtime.run(request);

  assert(result.termination_reason == execution::execution_termination_reason::policy_denied);
  assert(result.error_message.find("total input is too large") != std::string::npos);
  assert(result.metadata.at("code_bytes") == "4");
  assert(result.metadata.at("stdin_bytes") == "4");
  assert(result.metadata.at("max_total_input_bytes") == "7");
  assert(backend_ptr->calls == 0);
}

void test_policy_allows_input_at_exact_limits() {
  auto backend = std::make_unique<recording_backend>();
  auto* backend_ptr = backend.get();

  execution::execution_policy policy;
  policy.max_limits.max_code_bytes = 4;
  policy.max_limits.max_stdin_bytes = 3;
  policy.max_limits.max_total_input_bytes = 7;
  execution::execution_runtime runtime(std::move(backend), policy);

  execution::execution_request request;
  request.code = "1234";
  request.stdin_text = "123";
  const auto result = runtime.run(request);

  assert(result.termination_reason == execution::execution_termination_reason::exited);
  assert(backend_ptr->calls == 1);
}

void test_tool_provider_returns_clear_input_limit_error() {
  auto backend = std::make_unique<recording_backend>();
  auto* backend_ptr = backend.get();

  execution::execution_policy policy;
  policy.max_limits.max_code_bytes = 4;
  execution::execution_runtime runtime(std::move(backend), policy);
  execution::execution_tool_provider provider(runtime);

  nlohmann::json args {
    { "code", "12345" },
  };
  const auto result = provider.invoke("run_python_snippet", args.dump());

  assert(result.error_code);
  assert(backend_ptr->calls == 0);
  const auto content = nlohmann::json::parse(result.content);
  assert(content.at("termination_reason") == "policy_denied");
  assert(content.at("error_message").get<std::string>().find("code is too large") !=
         std::string::npos);
  assert(content.at("metadata").at("code_bytes") == "5");
  assert(content.at("metadata").at("max_code_bytes") == "4");
}

void test_controlled_process_backend_reports_launch_failure() {
  execution::controlled_process_backend backend({
    .python_interpreter = "definitely-not-a-real-python-interpreter",
    .fallback_workdir =
      std::filesystem::temp_directory_path() / "wuwe-execution-tests",
  });

  execution::execution_request request;
  request.code = "print(1)";
  request.limits.timeout = std::chrono::milliseconds(1000);

  const auto result = backend.run(request, {});
  assert(result.termination_reason == execution::execution_termination_reason::launch_failed);
  assert(!result.error_message.empty());
}

#ifdef WUWE_EXECUTION_TEST_PYTHON
void test_controlled_process_backend_runs_python_snippet() {
  execution::controlled_process_backend backend({
    .python_interpreter = WUWE_EXECUTION_TEST_PYTHON,
    .fallback_workdir =
      std::filesystem::temp_directory_path() / "wuwe-execution-tests",
  });

  execution::execution_request request;
  request.code =
    "import sys\n"
    "print('hello')\n"
    "print(sys.stdin.read(), end='')\n";
  request.stdin_text = "stdin-ok";
  request.limits.timeout = std::chrono::milliseconds(3000);

  const auto result = backend.run(request, {});
  assert(result.termination_reason == execution::execution_termination_reason::exited);
  assert(result.exit_code.has_value());
  assert(*result.exit_code == 0);
  assert(result.stdout_text.find("hello") != std::string::npos);
  assert(result.stdout_text.find("stdin-ok") != std::string::npos);
}

void test_controlled_process_backend_times_out_python() {
  execution::controlled_process_backend backend({
    .python_interpreter = WUWE_EXECUTION_TEST_PYTHON,
    .fallback_workdir =
      std::filesystem::temp_directory_path() / "wuwe-execution-tests",
  });

  execution::execution_request request;
  request.code = "while True:\n    pass\n";
  request.limits.timeout = std::chrono::milliseconds(100);

  const auto result = backend.run(request, {});
  assert(result.termination_reason == execution::execution_termination_reason::timeout);
  assert(result.timed_out);
}

void test_controlled_process_backend_truncates_stdout_and_stderr() {
  execution::controlled_process_backend backend({
    .python_interpreter = WUWE_EXECUTION_TEST_PYTHON,
    .fallback_workdir =
      std::filesystem::temp_directory_path() / "wuwe-execution-tests",
  });

  execution::execution_request request;
  request.code =
    "import sys\n"
    "sys.stdout.write('abcdef')\n"
    "sys.stderr.write('uvwxyz')\n";
  request.limits.timeout = std::chrono::milliseconds(3000);
  request.limits.max_stdout_bytes = 3;
  request.limits.max_stderr_bytes = 2;

  const auto result = backend.run(request, {});
  assert(result.termination_reason == execution::execution_termination_reason::exited);
  assert(result.stdout_text == "abc");
  assert(result.stderr_text == "uv");
  assert(result.stdout_truncated);
  assert(result.stderr_truncated);
}

void test_controlled_process_backend_cancels_python() {
  execution::controlled_process_backend backend({
    .python_interpreter = WUWE_EXECUTION_TEST_PYTHON,
    .fallback_workdir =
      std::filesystem::temp_directory_path() / "wuwe-execution-tests",
  });

  execution::execution_request request;
  request.code = "while True:\n    pass\n";
  request.limits.timeout = std::chrono::milliseconds(3000);

  std::stop_source stop_source;
  auto future = std::async(std::launch::async, [&]() {
    return backend.run(request, stop_source.get_token());
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  stop_source.request_stop();

  const auto result = future.get();
  assert(result.termination_reason == execution::execution_termination_reason::cancelled);
  assert(result.cancelled);
}

void test_controlled_process_backend_runs_concurrent_snippets() {
  execution::controlled_process_backend backend({
    .python_interpreter = WUWE_EXECUTION_TEST_PYTHON,
    .fallback_workdir =
      std::filesystem::temp_directory_path() / "wuwe-execution-tests",
  });

  auto run_one = [&](int id) {
    execution::execution_request request;
    request.code = "print('concurrent-" + std::to_string(id) + "')";
    request.limits.timeout = std::chrono::milliseconds(3000);
    return backend.run(request, {});
  };

  auto first = std::async(std::launch::async, run_one, 1);
  auto second = std::async(std::launch::async, run_one, 2);
  const auto first_result = first.get();
  const auto second_result = second.get();

  assert(first_result.termination_reason == execution::execution_termination_reason::exited);
  assert(second_result.termination_reason == execution::execution_termination_reason::exited);
  assert(first_result.stdout_text.find("concurrent-1") != std::string::npos);
  assert(second_result.stdout_text.find("concurrent-2") != std::string::npos);
}

void test_controlled_process_backend_job_kills_child_process_on_timeout() {
  const auto marker = std::filesystem::temp_directory_path() /
                      "wuwe-execution-child-survived.txt";
  std::error_code ignored;
  std::filesystem::remove(marker, ignored);

  execution::controlled_process_backend backend({
    .python_interpreter = WUWE_EXECUTION_TEST_PYTHON,
    .fallback_workdir =
      std::filesystem::temp_directory_path() / "wuwe-execution-tests",
    .use_job_object = true,
  });

  execution::execution_request request;
  const auto marker_text = escape_python_string(marker.string());
  request.code =
    "import subprocess, sys\n"
    "subprocess.Popen([sys.executable, '-c', "
    "\"import time, pathlib; time.sleep(1); "
    "pathlib.Path('" + marker_text + "').write_text('alive')\"])\n"
    "while True:\n"
    "    pass\n";
  request.limits.timeout = std::chrono::milliseconds(100);
  request.limits.max_process_count = 4;

  const auto result = backend.run(request, {});
  assert(result.termination_reason == execution::execution_termination_reason::timeout);
  assert(result.metadata.at("job_object_enabled") == "true");
  assert(result.metadata.at("process_tree_cleanup_enforcement") == "enforced");
  assert(result.metadata.at("process_count_limit_enforcement") == "enforced");
  assert(result.metadata.at("max_process_count") == "4");
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  assert(!std::filesystem::exists(marker));
}

void test_tool_provider_runs_python_through_controlled_backend() {
  execution::execution_policy policy;
  policy.default_workdir =
    std::filesystem::temp_directory_path() / "wuwe-execution-tests";
  policy.max_limits.timeout = std::chrono::milliseconds(3000);

  auto backend = execution::make_controlled_process_backend({
    .python_interpreter = WUWE_EXECUTION_TEST_PYTHON,
    .fallback_workdir =
      std::filesystem::temp_directory_path() / "wuwe-execution-tests",
  });
  execution::execution_runtime runtime(std::move(backend), policy);
  execution::execution_tool_provider provider(runtime);

  nlohmann::json args {
    { "code", "print('tool-python-ok')" },
    { "timeout_ms", 1000 },
  };
  const auto result = provider.invoke("run_python_snippet", args.dump());

  assert(!result.error_code);
  const auto content = nlohmann::json::parse(result.content);
  assert(content.at("termination_reason") == "exited");
  assert(content.at("stdout_text").get<std::string>().find("tool-python-ok") !=
         std::string::npos);
}
#endif

int main() {
  test_policy_denies_disallowed_language();
  test_runtime_clamps_limits_and_uses_env_allowlist();
  test_runtime_clamps_resource_limits_and_audits();
  test_policy_denies_invalid_allowed_environment_before_backend();
  test_approval_required_without_service_denies_before_backend();
  test_approval_allows_backend_and_audit_records_completion();
  test_tool_provider_exposes_narrow_schema_and_invokes_runtime();
  test_tool_provider_rejects_arguments_over_limit_before_parse();
  test_tool_provider_rejects_unknown_arguments_and_audits();
  test_tool_provider_rejects_timeout_over_schema_limit();
  test_runtime_audit_records_clamped_limits();
  test_default_backend_registry_exposes_controlled_process();
  test_backend_registry_selects_only_available_enforced_backends();
  test_planned_backend_descriptors_are_not_executable();
  test_controlled_process_contract_reflects_job_object_config();
  test_path_policy_rejects_prefix_trap();
  test_path_policy_handles_parent_traversal();
  test_runtime_normalizes_backend_exceptions();
  test_policy_denies_code_over_input_limit_before_backend();
  test_policy_denies_stdin_over_input_limit_before_backend();
  test_policy_denies_total_input_over_limit_before_backend();
  test_policy_allows_input_at_exact_limits();
  test_tool_provider_returns_clear_input_limit_error();
  test_controlled_process_backend_reports_launch_failure();
#ifdef WUWE_EXECUTION_TEST_PYTHON
  test_controlled_process_backend_runs_python_snippet();
  test_controlled_process_backend_times_out_python();
  test_controlled_process_backend_truncates_stdout_and_stderr();
  test_controlled_process_backend_cancels_python();
  test_controlled_process_backend_runs_concurrent_snippets();
  test_controlled_process_backend_job_kills_child_process_on_timeout();
  test_tool_provider_runs_python_through_controlled_backend();
#endif
}
