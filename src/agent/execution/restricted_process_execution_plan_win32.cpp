#include "restricted_process_execution_plan_win32.hpp"

#ifdef _WIN32

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string_view>
#include <utility>

#include "restricted_process_acl_win32.hpp"

namespace wuwe::agent::execution::detail {
namespace {

std::atomic<unsigned long long> plan_counter {};

restricted_execution_plan_result make_plan_result(
  restricted_execution_plan_status status,
  std::string detail = {}) {
  return {
    .status = status,
    .detail = std::move(detail),
  };
}

std::string next_profile_name() {
  const auto now =
    std::chrono::steady_clock::now().time_since_epoch().count();
  const auto counter = plan_counter.fetch_add(1, std::memory_order_relaxed);
  return "wuwe-restricted-exec-" + std::to_string(GetCurrentProcessId()) +
         "-" + std::to_string(now) + "-" + std::to_string(counter);
}

std::wstring widen_ascii(std::string_view text) {
  std::wstring result;
  result.reserve(text.size());
  for (const char ch : text) {
    result.push_back(static_cast<unsigned char>(ch));
  }
  return result;
}

std::string narrow_ascii(std::wstring_view text) {
  std::string result;
  result.reserve(text.size());
  for (const auto ch : text) {
    result.push_back(static_cast<char>(ch));
  }
  return result;
}

std::map<std::wstring, std::wstring> make_environment(
  const restricted_process_backend_config& config,
  const execution_request& request) {
  std::map<std::wstring, std::wstring> result;
  for (const auto& [name, value] : config.base_environment) {
    result.emplace(widen_ascii(name), widen_ascii(value));
  }
  for (const auto& [name, value] : request.env) {
    result[widen_ascii(name)] = widen_ascii(value);
  }
  return result;
}

std::filesystem::path workspace_root_for(
  const restricted_process_backend_config& config,
  const execution_request& request,
  const restricted_appcontainer_profile& profile) {
  if (!request.workdir.empty()) {
    return request.workdir;
  }
  if (!config.fallback_workdir.empty()) {
    return config.fallback_workdir;
  }
  return profile.storage_path() / "requests";
}

std::filesystem::path runtime_root_for(
  const restricted_process_backend_config& config,
  const restricted_appcontainer_profile& profile) {
  if (!config.runtime_staging_root.empty()) {
    return config.runtime_staging_root / profile.name() / "runtime";
  }
  return profile.storage_path() / "runtime";
}

restricted_execution_plan_result grant_tree_or_fail(
  const std::filesystem::path& path,
  PSID sid,
  DWORD directory_access,
  DWORD file_access,
  std::string_view label) {
  const auto grant = grant_restricted_tree_access({
    .path = path,
    .sid = sid,
    .directory_access = directory_access,
    .file_access = file_access,
  });
  if (grant.status != restricted_acl_grant_status::ok) {
    return make_plan_result(
      restricted_execution_plan_status::acl_grant_failed,
      std::string(label) + ": " + to_string(grant.status) + " " +
        grant.detail);
  }
  return {};
}

restricted_execution_plan_result grant_directory_or_fail(
  const std::filesystem::path& path,
  PSID sid,
  DWORD access,
  std::string_view label) {
  const auto grant = grant_restricted_directory_access(path, sid, access);
  if (grant.status != restricted_acl_grant_status::ok) {
    return make_plan_result(
      restricted_execution_plan_status::acl_grant_failed,
      std::string(label) + ": " + to_string(grant.status) + " " +
        grant.detail);
  }
  return {};
}

} // namespace

const char* to_string(restricted_execution_plan_status status) noexcept {
  switch (status) {
    case restricted_execution_plan_status::ok:
      return "ok";
    case restricted_execution_plan_status::unsupported_language:
      return "unsupported_language";
    case restricted_execution_plan_status::profile_failed:
      return "profile_failed";
    case restricted_execution_plan_status::runtime_staging_failed:
      return "runtime_staging_failed";
    case restricted_execution_plan_status::workspace_failed:
      return "workspace_failed";
    case restricted_execution_plan_status::acl_grant_failed:
      return "acl_grant_failed";
  }
  return "unknown";
}

restricted_execution_plan_result prepare_restricted_execution_plan(
  const restricted_process_backend_config& config,
  const execution_request& request) {
  if (request.language != execution_language::python) {
    return make_plan_result(
      restricted_execution_plan_status::unsupported_language);
  }

  auto profile_result = create_restricted_appcontainer_profile({
    .name = next_profile_name(),
    .display_name = L"Wuwe Restricted Execution",
    .description = L"Wuwe restricted execution AppContainer profile",
  });
  if (profile_result.status != restricted_appcontainer_profile_status::ok ||
      !profile_result.profile.has_value()) {
    return make_plan_result(
      restricted_execution_plan_status::profile_failed,
      to_string(profile_result.status) + std::string(" ") +
        profile_result.detail);
  }

  auto profile = std::move(*profile_result.profile);
  const auto runtime_root = runtime_root_for(config, profile);
  auto runtime_staging =
    stage_minimal_python_runtime_for_restricted_process({
      .source_python = config.python_interpreter,
      .destination_home = runtime_root,
      .replace_existing = true,
    });
  if (runtime_staging.status !=
      restricted_python_runtime_staging_status::ok) {
    return make_plan_result(
      restricted_execution_plan_status::runtime_staging_failed,
      to_string(runtime_staging.status) + std::string(" ") +
        runtime_staging.detail);
  }

  auto workspace_result = create_restricted_request_workspace({
    .root = workspace_root_for(config, request, profile),
    .script_text = request.code,
    .script_filename = "snippet.py",
    .cleanup_on_destroy = config.cleanup_runtime_staging,
  });
  if (workspace_result.status != restricted_request_workspace_status::ok ||
      !workspace_result.workspace.has_value()) {
    return make_plan_result(
      restricted_execution_plan_status::workspace_failed,
      to_string(workspace_result.status) + std::string(" ") +
        workspace_result.detail);
  }

  auto workspace = std::move(*workspace_result.workspace);
  constexpr DWORD read_execute = FILE_GENERIC_READ | FILE_GENERIC_EXECUTE;
  if (auto grant = grant_tree_or_fail(
        runtime_root,
        profile.sid(),
        read_execute,
        read_execute,
        "runtime");
      grant.status != restricted_execution_plan_status::ok) {
    return grant;
  }
  if (auto grant = grant_tree_or_fail(
        workspace.root(),
        profile.sid(),
        read_execute,
        read_execute,
        "workspace");
      grant.status != restricted_execution_plan_status::ok) {
    return grant;
  }
  for (const auto& root : config.readable_roots) {
    if (auto grant = grant_tree_or_fail(
          root,
          profile.sid(),
          read_execute,
          read_execute,
          "readable_root");
        grant.status != restricted_execution_plan_status::ok) {
      return grant;
    }
  }
  for (const auto& root : config.writable_roots) {
    if (auto grant = grant_directory_or_fail(
          root,
          profile.sid(),
          GENERIC_ALL,
          "writable_root");
        grant.status != restricted_execution_plan_status::ok) {
      return grant;
    }
  }

  restricted_appcontainer_launch_request launch_request {
    .executable = runtime_staging.python_executable,
    .appcontainer_sid = profile.sid(),
    .arguments = {
      L"-I",
      L"-S",
      workspace.script_path().wstring(),
    },
    .working_directory = workspace.root(),
    .stdin_text = request.stdin_text,
    .timeout = request.limits.timeout,
    .max_stdout_bytes = request.limits.max_stdout_bytes,
    .max_stderr_bytes = request.limits.max_stderr_bytes,
    .environment = make_environment(config, request),
  };
  const auto python_executable = runtime_staging.python_executable;

  restricted_execution_plan plan {
    .profile = std::move(profile),
    .workspace = std::move(workspace),
    .runtime_staging = std::move(runtime_staging),
    .launch_request = std::move(launch_request),
    .runtime_root = runtime_root,
    .python_executable = python_executable,
  };

  return {
    .status = restricted_execution_plan_status::ok,
    .plan = std::move(plan),
  };
}

execution_termination_reason termination_for_launch(
  const restricted_appcontainer_launch_result& launch) {
  if (launch.capture.timed_out) {
    return execution_termination_reason::timeout;
  }
  if (launch.capture.cancelled) {
    return execution_termination_reason::cancelled;
  }
  if (launch.status != restricted_appcontainer_launch_status::ok) {
    return execution_termination_reason::launch_failed;
  }
  return execution_termination_reason::exited;
}

void add_restricted_metadata(
  execution_result& result,
  const restricted_process_backend_config& config,
  const execution_request& request) {
  result.metadata["backend_name"] = "restricted_process";
  result.metadata["isolation_level"] = "restricted_process";
  result.metadata["backend_available"] = "false";
  result.metadata["backend_stage"] = "internal_execution_plan";
  result.metadata["python_interpreter"] = config.python_interpreter.string();
  result.metadata["deny_network"] = config.deny_network ? "true" : "false";
  result.metadata["use_job_object"] = config.use_job_object ? "true" : "false";
  result.metadata["inherit_parent_environment"] =
    config.inherit_parent_environment ? "true" : "false";
  result.metadata["cleanup_runtime_staging"] =
    config.cleanup_runtime_staging ? "true" : "false";
  result.metadata["readable_roots_count"] =
    std::to_string(config.readable_roots.size());
  result.metadata["writable_roots_count"] =
    std::to_string(config.writable_roots.size());
  result.metadata["timeout_ms"] = std::to_string(request.limits.timeout.count());
  result.metadata["max_stdout_bytes"] =
    std::to_string(request.limits.max_stdout_bytes);
  result.metadata["max_stderr_bytes"] =
    std::to_string(request.limits.max_stderr_bytes);
}

void add_plan_metadata(
  execution_result& result,
  const restricted_execution_plan& plan) {
  result.metadata["appcontainer_profile"] = narrow_ascii(plan.profile.name());
  result.metadata["workspace_root"] = plan.workspace.root().string();
  result.metadata["script_path"] = plan.workspace.script_path().string();
  result.metadata["runtime_root"] = plan.runtime_root.string();
  result.metadata["python_executable"] = plan.python_executable.string();
  result.metadata["runtime_staging_files"] =
    std::to_string(plan.runtime_staging.copied_files.size());
}

void cleanup_external_runtime_staging(
  const restricted_process_backend_config& config,
  const restricted_execution_plan& plan) {
  if (!config.cleanup_runtime_staging || config.runtime_staging_root.empty()) {
    return;
  }

  const auto cleanup_root = config.runtime_staging_root / plan.profile.name();
  std::error_code ignored;
  std::filesystem::remove_all(cleanup_root, ignored);
}

execution_result run_restricted_execution_plan(
  const restricted_process_backend_config& config,
  const execution_request& request,
  std::stop_token stop_token) {
  const auto started = std::chrono::steady_clock::now();
  execution_result result;
  add_restricted_metadata(result, config, request);

  auto plan_result = prepare_restricted_execution_plan(config, request);
  result.metadata["restricted_plan_status"] = to_string(plan_result.status);
  if (plan_result.status != restricted_execution_plan_status::ok ||
      !plan_result.plan.has_value()) {
    result.termination_reason =
      plan_result.status == restricted_execution_plan_status::unsupported_language
        ? execution_termination_reason::backend_error
        : execution_termination_reason::launch_failed;
    result.error_message =
      std::string("restricted execution plan failed: ") +
      to_string(plan_result.status) + " " + plan_result.detail;
    result.metadata["error_code"] =
      std::string("restricted_plan_") + to_string(plan_result.status);
    result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);
    return result;
  }

  auto& plan = *plan_result.plan;
  add_plan_metadata(result, plan);
  plan.launch_request.stop_token = stop_token;
  auto launch = launch_restricted_appcontainer_process(
    std::move(plan.launch_request));

  result.exit_code = static_cast<int>(launch.capture.exit_code);
  result.timed_out = launch.capture.timed_out;
  result.cancelled = launch.capture.cancelled;
  result.stdout_truncated = launch.capture.stdout_truncated;
  result.stderr_truncated = launch.capture.stderr_truncated;
  result.stdout_text = std::move(launch.capture.stdout_text);
  result.stderr_text = std::move(launch.capture.stderr_text);
  result.termination_reason = termination_for_launch(launch);
  result.metadata["restricted_launch_status"] = to_string(launch.status);
  result.metadata["restricted_launch_win32_error"] =
    std::to_string(launch.win32_error);
  if (!launch.detail.empty()) {
    result.metadata["restricted_launch_detail"] = launch.detail;
  }
  if (launch.status != restricted_appcontainer_launch_status::ok) {
    result.error_message =
      std::string("restricted launch failed: ") + to_string(launch.status);
    result.metadata["error_code"] =
      std::string("restricted_launch_") + to_string(launch.status);
  }
  else if (result.timed_out) {
    result.error_message = "restricted execution timed out";
    result.metadata["error_code"] = "restricted_execution_timeout";
  }
  else if (result.cancelled) {
    result.error_message = "restricted execution cancelled";
    result.metadata["error_code"] = "restricted_execution_cancelled";
  }

  cleanup_external_runtime_staging(config, plan);
  result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now() - started);
  return result;
}

} // namespace wuwe::agent::execution::detail

#endif // _WIN32
