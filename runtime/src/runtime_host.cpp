#include <wuwe/runtime_host/runtime_host.hpp>

#include <algorithm>
#include <condition_variable>
#include <future>
#include <mutex>
#include <random>
#include <stop_token>
#include <unordered_map>
#include <utility>

#include <wuwe/agent/host/host_codec.hpp>
#include <wuwe/agent/host/host_dispatcher.hpp>
#include <wuwe/agent/llm/llm_agent_runner.h>
#include <wuwe/agent/llm/llm_config.h>
#include <wuwe/agent/llm/llm_provider_factory.h>
#include <wuwe/agent/llm/llm_provider_registry.h>
#include <wuwe/agent/runtime/run_runtime.hpp>
#include <wuwe/agent/runtime/sqlite_agent_run_store.hpp>
#include <wuwe/common/sha256.hpp>

#include "idempotency_repository.hpp"

namespace wuwe::runtime_host {
namespace {

using agent::host::host_error;
using agent::host::host_error_code;
using agent::runtime::agent_run_status;
using agent::runtime::run_store_write_result;
using agent::runtime::run_store_write_status;

std::string make_run_id() {
  static constexpr char alphabet[] = "0123456789abcdef";
  std::random_device source;
  std::string result("run-");
  result.reserve(36);
  for (std::size_t index = 0; index < 16; ++index) {
    const auto value = static_cast<unsigned int>(source());
    result.push_back(alphabet[(value >> 4U) & 0x0fU]);
    result.push_back(alphabet[value & 0x0fU]);
  }
  return result;
}

std::shared_ptr<agent::runtime::agent_run_store> default_run_store(
  const run_store_options& options) {
  if (options.kind == run_store_kind::memory)
    return std::make_shared<agent::runtime::in_memory_agent_run_store>();
#if WUWE_HAS_SQLITE
  return std::make_shared<agent::runtime::sqlite_agent_run_store>(options.sqlite_path);
#else
  (void)options;
  throw runtime_error(runtime_error_code::dependency_unavailable,
    "SQLite run storage is not available in this Wuwe build");
#endif
}

std::shared_ptr<llm_client> default_llm_client(const llm_options& options) {
  const auto* provider = find_llm_provider(options.provider);
  if (!provider) {
    throw runtime_error(runtime_error_code::dependency_unavailable,
      "LLM provider is not registered: " + options.provider);
  }
  auto client_config = make_default_llm_config(*provider);
  if (!options.base_url.empty())
    client_config.base_url = options.base_url;
  if (provider->base_url_required && client_config.base_url.empty()) {
    throw runtime_error(runtime_error_code::dependency_unavailable,
      "LLM provider requires a baseUrl: " + options.provider);
  }
  client_config.api_key.clear();
  client_config.model = options.model;
  client_config.timeout = static_cast<int>(options.timeout.count());
  client_config.max_retries = options.max_retries;
  client_config.load_api_key_from_environment = false;
  if (!options.chat_completions_path.empty())
    client_config.chat_completions_path = options.chat_completions_path;
  if (!options.api_key_env.empty())
    client_config.api_key = llm_client_config::load_env_value(options.api_key_env.c_str());
  if (client_config.require_api_key && client_config.api_key.empty()) {
    throw runtime_error(runtime_error_code::dependency_unavailable,
      "required LLM credential environment variable is not set: " + options.api_key_env);
  }
  auto client = make_llm_client(options.provider, std::move(client_config));
  if (!client) {
    throw runtime_error(runtime_error_code::dependency_unavailable,
      "LLM provider is not registered: " + options.provider);
  }
  return client;
}

std::shared_ptr<llm_agent_runner> default_agent_runner(llm_client& client) {
  return std::make_shared<llm_agent_runner>(client);
}

host_error write_error(const run_store_write_result& write, std::string_view operation) {
  switch (write.status) {
    case run_store_write_status::not_found:
      return {
        .code = host_error_code::not_found,
        .message = "agent run was not found",
      };
    case run_store_write_status::conflict:
      return {
        .code = host_error_code::conflict,
        .message = "agent run revision conflict",
        .retryable = true,
        .details = { { "actualRevision", write.revision } },
      };
    case run_store_write_status::already_exists:
      return {
        .code = host_error_code::conflict,
        .message = "agent run already exists",
        .details = { { "actualRevision", write.revision } },
      };
    case run_store_write_status::applied:
      break;
  }
  return {
    .code = host_error_code::internal,
    .message = std::string(operation) + " failed",
  };
}

template<typename T>
agent::host::host_result<T> host_failure(host_error_code code, std::string message,
  bool retryable = false, nlohmann::json details = nlohmann::json::object()) {
  return agent::host::host_result<T>::failure({
    .code = code,
    .message = std::move(message),
    .retryable = retryable,
    .details = std::move(details),
  });
}

std::string required_prompt(const nlohmann::json& input) {
  if (!input.is_object())
    throw std::invalid_argument("create_run input must be an object");
  const auto found = input.find("prompt");
  if (found == input.end() || !found->is_string() || found->get_ref<const std::string&>().empty())
    throw std::invalid_argument("create_run input requires a non-empty string 'prompt'");
  if (input.size() != 1)
    throw std::invalid_argument("create_run input supports only the 'prompt' field");
  return found->get<std::string>();
}

void merge_run_metadata(agent::core::agent_execution_context& context,
  const std::map<std::string, std::string>& metadata) {
  for (const auto& [key, value] : metadata) {
    if (agent::core::sensitive_execution_context_metadata_key(key)) {
      throw std::invalid_argument("create_run metadata must not contain sensitive key: " + key);
    }
    const auto [found, inserted] = context.metadata.emplace(key, value);
    if (!inserted && found->second != value) {
      throw std::invalid_argument(
        "create_run metadata conflicts with execution context metadata: " + key);
    }
  }
}

std::string idempotency_fingerprint(const std::map<std::string, std::string>& metadata,
  std::string_view request_fingerprint) {
  common::sha256 hash;
  const auto encoded_metadata = nlohmann::json(metadata).dump();
  for (const auto field : { std::string_view(encoded_metadata), request_fingerprint }) {
    hash.update(std::to_string(field.size()));
    hash.update(":");
    hash.update(field);
  }
  return hash.hex_digest();
}

nlohmann::json submission_result_to_json(
  const agent::host::host_result<agent::host::run_submission>& result) {
  if (const auto* value = result.value_if()) {
    return {
      { "ok", true },
      { "value", agent::host::run_submission_to_json(*value) },
    };
  }
  return {
    { "ok", false },
    { "error", agent::host::host_error_to_json(*result.error_if()) },
  };
}

agent::host::host_result<agent::host::run_submission> submission_result_from_json(
  const nlohmann::json& document) {
  if (!document.is_object() || !document.contains("ok") || !document.at("ok").is_boolean())
    throw std::runtime_error("persisted idempotency result is malformed");
  if (document.at("ok").get<bool>()) {
    return agent::host::host_result<agent::host::run_submission>::success(
      agent::host::run_submission_from_json(document.at("value")));
  }
  return agent::host::host_result<agent::host::run_submission>::failure(
    agent::host::host_error_from_json(document.at("error")));
}

} // namespace

std::string to_string(runtime_error_code code) {
  switch (code) {
    case runtime_error_code::invalid_state:
      return "invalid_state";
    case runtime_error_code::dependency_unavailable:
      return "dependency_unavailable";
    case runtime_error_code::capacity_exceeded:
      return "capacity_exceeded";
    case runtime_error_code::run_conflict:
      return "run_conflict";
    case runtime_error_code::execution_failed:
      return "execution_failed";
  }
  return "execution_failed";
}

std::string to_string(health_status status) {
  switch (status) {
    case health_status::healthy:
      return "healthy";
    case health_status::degraded:
      return "degraded";
    case health_status::unhealthy:
      return "unhealthy";
  }
  return "unhealthy";
}

runtime_error::runtime_error(runtime_error_code code, std::string message)
    : std::runtime_error(std::move(message)), code_(code) {
}

doctor_report diagnose_runtime_config(
  const runtime_config& config, const runtime_dependencies& dependencies) {
  doctor_report report;
  const auto validation = validate_runtime_config(runtime_config_to_json(config));
  report.checks["config"] = {
    .ok = validation.valid(),
    .message = validation.valid() ? "runtime configuration is valid"
                                  : "runtime configuration failed schema validation",
  };

  const bool custom_client = static_cast<bool>(dependencies.make_llm_client);
  const auto* provider = custom_client ? nullptr : find_llm_provider(config.llm.provider);
  report.checks["provider"] = {
    .ok = custom_client || provider != nullptr,
    .message = custom_client ? "custom LLM client factory is configured"
                             : (provider ? "built-in LLM provider is available"
                                         : "LLM provider requires external factory registration"),
  };
  const bool endpoint_available =
    custom_client || !provider || !provider->base_url_required || !config.llm.base_url.empty() ||
    !provider->default_base_url.empty();
  report.checks["endpoint"] = {
    .ok = endpoint_available,
    .message = custom_client ? "endpoint resolution is delegated to the custom LLM client factory"
                             : (endpoint_available ? "LLM endpoint requirement is satisfied"
                                                   : "LLM provider requires baseUrl"),
  };
  const bool key_required = !custom_client && (!provider || provider->api_key_required);
  const bool credential_available =
    !key_required || (!config.llm.api_key_env.empty() &&
                       !llm_client_config::load_env_value(config.llm.api_key_env.c_str()).empty());
  report.checks["credential"] = {
    .ok = credential_available,
    .message = custom_client ? "credential resolution is delegated to the custom LLM client factory"
                             : (credential_available
                                   ? "LLM credential requirement is satisfied"
                                   : "required LLM credential environment variable is not set"),
  };

  bool store_available = true;
  std::string store_message = dependencies.make_run_store ? "custom run store factory is configured"
                                                          : "in-memory run store is available";
  if (!dependencies.make_run_store && config.store.kind == run_store_kind::sqlite) {
#if WUWE_HAS_SQLITE
    store_message = "SQLite run store support is available";
#else
    store_available = false;
    store_message = "SQLite run store support is not compiled into this Wuwe build";
#endif
  }
  report.checks["store"] = { .ok = store_available, .message = std::move(store_message) };
  report.ready = std::all_of(
    report.checks.begin(), report.checks.end(), [](const auto& item) { return item.second.ok; });
  return report;
}

class host::implementation final : public agent::host::agent_host_service {
public:
  explicit implementation(runtime_dependencies dependencies)
      : uses_default_run_store_(!dependencies.make_run_store),
        dependencies_(std::move(dependencies)) {
    if (!dependencies_.make_llm_client)
      dependencies_.make_llm_client = default_llm_client;
    if (!dependencies_.make_agent_runner)
      dependencies_.make_agent_runner = default_agent_runner;
    if (!dependencies_.make_run_store)
      dependencies_.make_run_store = default_run_store;
  }

  ~implementation() override {
    if (!stop(std::nullopt))
      shutdown_for_destruction();
  }

  void configure(runtime_config config) {
    std::scoped_lock control_lock(control_mutex_);
    if (lifecycle_.state() != lifecycle_state::created) {
      throw runtime_error(runtime_error_code::invalid_state, "runtime can only be configured once");
    }
    auto report = validate_runtime_config(runtime_config_to_json(config));
    if (!report)
      throw validation_error("runtime configuration", std::move(report));
    config_ = std::move(config);
    lifecycle_.transition(lifecycle_state::configured);
  }

  void start() {
    std::scoped_lock control_lock(control_mutex_);
    if (lifecycle_.state() != lifecycle_state::configured) {
      throw runtime_error(
        runtime_error_code::invalid_state, "runtime must be configured before start");
    }
    lifecycle_.transition(lifecycle_state::starting);
    try {
      auto store = dependencies_.make_run_store(config_->store);
      if (!store)
        throw runtime_error(
          runtime_error_code::dependency_unavailable, "run store factory returned no store");
      auto durable = std::make_shared<agent::runtime::agent_run_runtime>(store);
      auto idempotency_repository =
        uses_default_run_store_ && config_->store.kind == run_store_kind::sqlite
          ? detail::make_sqlite_idempotency_repository(
              config_->store.sqlite_path, max_idempotency_entries)
          : detail::make_memory_idempotency_repository(max_idempotency_entries);
      auto client = dependencies_.make_llm_client(config_->llm);
      if (!client)
        throw runtime_error(
          runtime_error_code::dependency_unavailable, "LLM client factory returned no client");
      auto runner = dependencies_.make_agent_runner(*client);
      if (!runner)
        throw runtime_error(
          runtime_error_code::dependency_unavailable, "agent runner factory returned no runner");
      {
        std::scoped_lock lock(resources_mutex_);
        store_ = std::move(store);
        durable_runtime_ = std::move(durable);
        idempotency_repository_ = std::move(idempotency_repository);
        client_ = std::move(client);
        runner_ = std::move(runner);
      }
      lifecycle_.transition(lifecycle_state::ready);
    }
    catch (const std::exception& ex) {
      lifecycle_.fail(ex.what());
      throw;
    }
    catch (...) {
      lifecycle_.fail("runtime startup failed");
      throw;
    }
  }

  bool stop(std::optional<std::chrono::milliseconds> timeout) noexcept {
    try {
      std::scoped_lock control_lock(control_mutex_);
      const auto state = lifecycle_.state();
      if (state == lifecycle_state::stopped)
        return true;
      if (state == lifecycle_state::failed)
        return false;
      if (state == lifecycle_state::created || state == lifecycle_state::configured) {
        lifecycle_.transition(lifecycle_state::stopped);
        return true;
      }
      if (state == lifecycle_state::ready || state == lifecycle_state::starting)
        lifecycle_.transition(lifecycle_state::draining);

      std::unique_lock lock(active_mutex_);
      for (const auto& [_, source] : active_runs_)
        source->request_stop();
      const auto wait = timeout.value_or(
        config_ ? config_->runtime.shutdown_timeout : std::chrono::milliseconds(10000));
      if (!active_changed_.wait_for(lock, wait, [&] { return active_runs_.empty(); })) {
        lock.unlock();
        lifecycle_.fail("runtime shutdown timed out while runs were still active");
        return false;
      }
      lock.unlock();
      {
        std::scoped_lock background_lock(background_mutex_);
        background_runs_.clear();
      }
      {
        std::scoped_lock resources_lock(resources_mutex_);
        runner_.reset();
        client_.reset();
        idempotency_repository_.reset();
        durable_runtime_.reset();
        store_.reset();
      }
      lifecycle_.transition(lifecycle_state::stopped);
      return true;
    }
    catch (...) {
      lifecycle_.fail("runtime shutdown failed");
      return false;
    }
  }

  lifecycle_snapshot lifecycle_status() const {
    return lifecycle_.snapshot();
  }

  const runtime_config& config() const {
    if (!config_)
      throw runtime_error(runtime_error_code::invalid_state, "runtime is not configured");
    return *config_;
  }

  health_snapshot health() const {
    health_snapshot result;
    result.lifecycle = lifecycle_.snapshot();
    const bool ready = result.lifecycle.state == lifecycle_state::ready;
    bool has_client = false;
    bool has_store = false;
    bool has_idempotency = false;
    {
      std::scoped_lock lock(resources_mutex_);
      has_client = static_cast<bool>(client_);
      has_store = static_cast<bool>(store_);
      has_idempotency = static_cast<bool>(idempotency_repository_);
    }
    result.checks["lifecycle"] = {
      .ok = ready,
      .message = ready ? "runtime is accepting runs"
                       : "runtime lifecycle is " + to_string(result.lifecycle.state),
    };
    result.checks["llm"] = {
      .ok = has_client,
      .message = has_client ? "LLM client is configured" : "LLM client is unavailable",
    };
    result.checks["store"] = {
      .ok = has_store,
      .message = has_store ? "durable run store is configured" : "durable run store is unavailable",
    };
    result.checks["idempotency"] = {
      .ok = has_idempotency,
      .message = has_idempotency ? "mutation replay repository is configured"
                                 : "mutation replay repository is unavailable",
    };
    if (ready && has_client && has_store && has_idempotency)
      result.status = health_status::healthy;
    else if (result.lifecycle.state == lifecycle_state::starting ||
             result.lifecycle.state == lifecycle_state::draining)
      result.status = health_status::degraded;
    else
      result.status = health_status::unhealthy;
    return result;
  }

  run_result run(std::string prompt, agent::core::agent_execution_context context) {
    if (prompt.empty())
      throw std::invalid_argument("runtime run requires a non-empty prompt");
    ensure_ready();
    if (context.run_id.empty())
      context.run_id = make_run_id();
    if (!context.deadline)
      context.deadline = std::chrono::system_clock::now() + config_->runtime.default_run_timeout;
    if (const auto durable = durable_runtime(); durable && durable->get(context.run_id)) {
      throw runtime_error(
        runtime_error_code::run_conflict, "agent run already exists: " + context.run_id);
    }

    auto stop_source = reserve_run(context.run_id);
    return execute_reserved_run(std::move(prompt), std::move(context), std::move(stop_source));
  }

  run_result execute_reserved_run(std::string prompt, agent::core::agent_execution_context context,
    std::shared_ptr<std::stop_source> stop_source) {
    const auto run_id = context.run_id;
    const auto durable = durable_runtime();
    const auto runner = agent_runner();
    if (!durable || !runner) {
      release_run(run_id);
      throw runtime_error(runtime_error_code::dependency_unavailable,
        "runtime execution dependencies are unavailable");
    }
    struct release_guard {
      implementation& owner;
      std::string run_id;
      ~release_guard() {
        owner.release_run(run_id);
      }
    } release { *this, run_id };

    try {
      std::stop_source linked_stop_source;
      std::stop_callback host_stop_callback(
        stop_source->get_token(), [&linked_stop_source] { linked_stop_source.request_stop(); });
      std::stop_callback caller_stop_callback(
        context.stop_token, [&linked_stop_source] { linked_stop_source.request_stop(); });
      if (stop_source->stop_requested() || context.stop_token.stop_requested())
        linked_stop_source.request_stop();
      llm_agent_run_options options {
        .stop_token = linked_stop_source.get_token(),
        .context = std::move(context),
        .runtime = durable,
      };
      auto response = runner->complete(prompt, std::move(options));
      const auto stored_run_id =
        response.metadata.contains("run_id") ? response.metadata.at("run_id") : release.run_id;
      const auto record = durable->get(stored_run_id);
      if (!record) {
        throw runtime_error(runtime_error_code::execution_failed,
          "LLM execution did not produce a durable run record");
      }
      return {
        .run_id = record->id,
        .revision = record->revision,
        .status = record->status,
        .content = response.content,
        .error = record->error,
        .result = record->result,
        .metadata = response.metadata,
      };
    }
    catch (const runtime_error&) {
      fail_incomplete_run(release.run_id, "runtime execution failed");
      throw;
    }
    catch (const std::exception& ex) {
      fail_incomplete_run(release.run_id, ex.what());
      throw runtime_error(runtime_error_code::execution_failed, ex.what());
    }
    catch (...) {
      fail_incomplete_run(release.run_id, "runtime execution failed");
      throw runtime_error(runtime_error_code::execution_failed, "runtime execution failed");
    }
  }

  bool cancel_run(const std::string& run_id) {
    std::shared_ptr<std::stop_source> source;
    {
      std::scoped_lock lock(active_mutex_);
      const auto found = active_runs_.find(run_id);
      if (found == active_runs_.end())
        return false;
      source = found->second;
    }
    return source->request_stop();
  }

  std::optional<agent::host::run_view> get_run_view(const std::string& run_id) const {
    const auto durable = durable_runtime();
    if (!durable)
      return std::nullopt;
    const auto record = durable->get(run_id);
    return record ? std::optional(agent::host::run_view_from_runtime(*record)) : std::nullopt;
  }

  nlohmann::json dispatch(const nlohmann::json& request) {
    agent::host::host_dispatcher dispatcher(*this);
    return dispatcher.dispatch_json(request);
  }

  agent::host::host_result<agent::host::run_submission> create_run(
    const agent::host::host_call_context& call,
    const agent::host::create_run_request& request) override {
    return idempotent_submission(
      call, "create_run", agent::host::create_run_request_to_json(request).dump(), [&] {
        try {
          auto context = request.context;
          merge_run_metadata(context, request.metadata);
          return submit_background_run(required_prompt(request.input), std::move(context));
        }
        catch (const std::invalid_argument& ex) {
          return host_failure<agent::host::run_submission>(
            host_error_code::invalid_request, ex.what());
        }
        catch (const runtime_error& ex) {
          const bool capacity = ex.code() == runtime_error_code::capacity_exceeded;
          if (ex.code() == runtime_error_code::run_conflict) {
            return host_failure<agent::host::run_submission>(host_error_code::conflict, ex.what());
          }
          return host_failure<agent::host::run_submission>(host_error_code::unavailable,
            capacity ? ex.what() : "runtime could not execute the run",
            capacity);
        }
      });
  }

  agent::host::host_result<agent::host::run_view> get_run(
    const agent::host::host_call_context&, const agent::host::get_run_request& request) override {
    const auto result = get_run_view(request.run_id);
    if (!result)
      return host_failure<agent::host::run_view>(
        host_error_code::not_found, "agent run was not found");
    return agent::host::host_result<agent::host::run_view>::success(*result);
  }

  agent::host::host_result<agent::host::run_submission> cancel_run(
    const agent::host::host_call_context& call,
    const agent::host::cancel_run_request& request) override {
    return idempotent_submission(
      call, "cancel_run", agent::host::cancel_run_request_to_json(request).dump(), [&] {
        const auto durable = durable_runtime();
        const auto record = durable ? durable->get(request.run_id) : std::nullopt;
        if (!record) {
          return host_failure<agent::host::run_submission>(
            host_error_code::not_found, "agent run was not found");
        }
        if (record->revision != request.expected_revision) {
          return host_failure<agent::host::run_submission>(host_error_code::conflict,
            "agent run revision conflict",
            true,
            { { "actualRevision", record->revision } });
        }
        if (agent::runtime::terminal(record->status)) {
          return agent::host::host_result<agent::host::run_submission>::success({
            .run_id = record->id,
            .revision = record->revision,
            .status = record->status,
          });
        }
        if (cancel_run(request.run_id)) {
          return agent::host::host_result<agent::host::run_submission>::success({
            .run_id = record->id,
            .revision = record->revision,
            .status = record->status,
          });
        }
        const auto write =
          durable->cancel(request.run_id, request.expected_revision, request.reason);
        if (!write) {
          return agent::host::host_result<agent::host::run_submission>::failure(
            write_error(write, "cancel_run"));
        }
        return agent::host::host_result<agent::host::run_submission>::success({
          .run_id = request.run_id,
          .revision = write.revision,
          .status = agent_run_status::cancelled,
        });
      });
  }

  agent::host::host_result<agent::host::run_submission> resolve_approval(
    const agent::host::host_call_context& call,
    const agent::host::resolve_approval_request& request) override {
    return idempotent_submission(
      call, "resolve_approval", agent::host::resolve_approval_request_to_json(request).dump(), [&] {
        for (const auto& [key, _] : request.metadata) {
          if (agent::core::sensitive_metadata_key(key)) {
            return host_failure<agent::host::run_submission>(host_error_code::invalid_request,
              "resolve_approval metadata must not contain sensitive key: " + key);
          }
        }
        const auto durable = durable_runtime();
        const auto record = durable ? durable->get(request.run_id) : std::nullopt;
        if (!record) {
          return host_failure<agent::host::run_submission>(
            host_error_code::not_found, "agent run was not found");
        }
        if (!record->suspension || record->suspension->approval_id != request.approval_id) {
          return host_failure<agent::host::run_submission>(
            host_error_code::conflict, "agent run is not waiting for the specified approval");
        }
        try {
          const auto write = durable->resolve_approval(request.run_id,
            request.expected_revision,
            record->suspension->continuation_token,
            request.resolution,
            request.reason,
            request.metadata);
          if (!write) {
            return agent::host::host_result<agent::host::run_submission>::failure(
              write_error(write, "resolve_approval"));
          }
          const auto updated = durable->get(request.run_id);
          return agent::host::host_result<agent::host::run_submission>::success({
            .run_id = request.run_id,
            .revision = write.revision,
            .status = updated ? updated->status : agent_run_status::failed,
          });
        }
        catch (const std::logic_error& ex) {
          return host_failure<agent::host::run_submission>(host_error_code::conflict, ex.what());
        }
      });
  }

  agent::host::host_result<agent::host::run_submission> resume_run(
    const agent::host::host_call_context& call,
    const agent::host::resume_run_request& request) override {
    return idempotent_submission(
      call, "resume_run", agent::host::resume_run_request_to_json(request).dump(), [&] {
        if (!request.input.is_object() || !request.input.empty()) {
          return host_failure<agent::host::run_submission>(host_error_code::invalid_request,
            "resume_run input is reserved and must be an empty object in Runtime API version 1");
        }
        const auto durable = durable_runtime();
        const auto record = durable ? durable->get(request.run_id) : std::nullopt;
        if (!record) {
          return host_failure<agent::host::run_submission>(
            host_error_code::not_found, "agent run was not found");
        }
        const auto continuation =
          record->suspension ? record->suspension : record->active_continuation;
        if (!continuation || continuation->approval_id != request.approval_id ||
            continuation->resolution != agent::runtime::approval_resolution::approved) {
          return host_failure<agent::host::run_submission>(host_error_code::conflict,
            "agent run has no approved continuation for the specified approval");
        }
        if (record->revision != request.expected_revision) {
          return host_failure<agent::host::run_submission>(host_error_code::conflict,
            "agent run revision conflict",
            true,
            { { "actualRevision", record->revision } });
        }
        try {
          return submit_background_resume(*record, *continuation, request, durable);
        }
        catch (const runtime_error& ex) {
          if (ex.code() == runtime_error_code::run_conflict) {
            return host_failure<agent::host::run_submission>(host_error_code::conflict, ex.what());
          }
          const bool retryable = ex.code() == runtime_error_code::capacity_exceeded ||
                                 ex.code() == runtime_error_code::invalid_state;
          return host_failure<agent::host::run_submission>(
            host_error_code::unavailable, ex.what(), retryable);
        }
      });
  }

  agent::host::host_result<agent::host::event_page> list_events(
    const agent::host::host_call_context&,
    const agent::host::list_events_request& request) override {
    const auto durable = durable_runtime();
    if (!durable || !durable->get(request.run_id))
      return host_failure<agent::host::event_page>(
        host_error_code::not_found, "agent run was not found");
    auto events = durable->list_events(request.run_id, request.after_sequence);
    agent::host::event_page page;
    page.has_more = events.size() > request.limit;
    if (page.has_more)
      events.resize(request.limit);
    page.events.reserve(events.size());
    for (const auto& event : events)
      page.events.push_back(agent::host::host_event_from_runtime(event));
    page.next_sequence = page.events.empty() ? request.after_sequence : page.events.back().sequence;
    return agent::host::host_result<agent::host::event_page>::success(std::move(page));
  }

private:
  struct idempotency_entry {
    std::string fingerprint;
    std::chrono::steady_clock::time_point created_at { std::chrono::steady_clock::now() };
    std::optional<agent::host::host_result<agent::host::run_submission>> result;
    std::condition_variable completed;
  };

  template<typename Callable>
  agent::host::host_result<agent::host::run_submission> idempotent_submission(
    const agent::host::host_call_context& call, std::string_view operation,
    std::string request_fingerprint, Callable&& invoke) {
    if (call.idempotency_key.empty()) {
      return host_failure<agent::host::run_submission>(
        host_error_code::invalid_request, "mutating runtime request requires an idempotency key");
    }
    for (const auto& [metadata_key, _] : call.metadata) {
      if (agent::core::sensitive_metadata_key(metadata_key)) {
        return host_failure<agent::host::run_submission>(host_error_code::invalid_request,
          "runtime call metadata must not contain sensitive key: " + metadata_key);
      }
    }
    const auto key =
      call.protocol_version + ":" + std::string(operation) + ":" + call.idempotency_key;
    const auto fingerprint = idempotency_fingerprint(call.metadata, request_fingerprint);
    const auto repository = idempotency_repository();
    if (!repository) {
      return host_failure<agent::host::run_submission>(
        host_error_code::unavailable, "runtime idempotency repository is unavailable", true);
    }
    std::shared_ptr<idempotency_entry> entry;
    {
      std::unique_lock lock(idempotency_mutex_);
      if (const auto found = idempotency_.find(key); found != idempotency_.end()) {
        entry = found->second;
        if (entry->fingerprint != fingerprint) {
          return host_failure<agent::host::run_submission>(
            host_error_code::conflict, "idempotency key was already used for a different request");
        }
        entry->completed.wait(lock, [&] { return entry->result.has_value(); });
        return *entry->result;
      }
      try {
        const auto claim = repository->claim(key, fingerprint);
        switch (claim.status) {
          case detail::idempotency_claim_status::replay:
            return submission_result_from_json(claim.result);
          case detail::idempotency_claim_status::conflict:
            return host_failure<agent::host::run_submission>(host_error_code::conflict,
              "idempotency key was already used for a different request");
          case detail::idempotency_claim_status::pending:
            return host_failure<agent::host::run_submission>(host_error_code::unavailable,
              "a previous mutation with this idempotency key has an unresolved outcome",
              true);
          case detail::idempotency_claim_status::capacity_exceeded:
            return host_failure<agent::host::run_submission>(host_error_code::unavailable,
              "runtime idempotency repository is at capacity",
              true);
          case detail::idempotency_claim_status::acquired:
            break;
        }
      }
      catch (...) {
        return host_failure<agent::host::run_submission>(
          host_error_code::unavailable, "runtime idempotency repository could not be read", true);
      }
      prune_idempotency_cache();
      if (idempotency_.size() >= max_idempotency_entries) {
        try {
          repository->erase(key, fingerprint);
        }
        catch (...) {
          return host_failure<agent::host::run_submission>(host_error_code::internal,
            "runtime idempotency claim could not be released");
        }
        return host_failure<agent::host::run_submission>(
          host_error_code::unavailable, "runtime idempotency cache is at capacity", true);
      }
      entry = std::make_shared<idempotency_entry>();
      entry->fingerprint = fingerprint;
      idempotency_.emplace(key, entry);
    }

    agent::host::host_result<agent::host::run_submission> result = [&] {
      try {
        return std::forward<Callable>(invoke)();
      }
      catch (...) {
        return host_failure<agent::host::run_submission>(
          host_error_code::internal, "runtime mutation failed");
      }
    }();
    bool erase_replay = false;
    try {
      if (const auto* error = result.error_if();
          error && error->retryable && error->code == host_error_code::unavailable) {
        repository->erase(key, fingerprint);
        erase_replay = true;
      }
      else {
        repository->complete(key, fingerprint, submission_result_to_json(result));
      }
    }
    catch (...) {
      result = host_failure<agent::host::run_submission>(host_error_code::internal,
        "runtime mutation outcome could not be persisted");
      erase_replay = false;
    }
    {
      std::scoped_lock lock(idempotency_mutex_);
      entry->result = result;
      if (erase_replay)
        idempotency_.erase(key);
    }
    entry->completed.notify_all();
    return result;
  }

  void prune_idempotency_cache() {
    while (idempotency_.size() >= max_idempotency_entries) {
      auto oldest = idempotency_.end();
      for (auto current = idempotency_.begin(); current != idempotency_.end(); ++current) {
        if (!current->second->result)
          continue;
        if (oldest == idempotency_.end() ||
            current->second->created_at < oldest->second->created_at)
          oldest = current;
      }
      if (oldest == idempotency_.end())
        return;
      idempotency_.erase(oldest);
    }
  }

  void ensure_ready() const {
    if (lifecycle_.state() != lifecycle_state::ready) {
      throw runtime_error(runtime_error_code::invalid_state, "runtime is not accepting new runs");
    }
  }

  agent::host::host_result<agent::host::run_submission> submit_background_run(
    std::string prompt, agent::core::agent_execution_context context) {
    ensure_ready();
    if (context.run_id.empty())
      context.run_id = make_run_id();
    const auto run_id = context.run_id;
    reap_background_runs();
    if (const auto durable = durable_runtime(); durable && durable->get(run_id)) {
      return host_failure<agent::host::run_submission>(
        host_error_code::conflict, "agent run already exists");
    }

    std::shared_future<run_result> future;
    {
      std::scoped_lock lock(background_mutex_);
      if (lifecycle_.state() != lifecycle_state::ready) {
        return host_failure<agent::host::run_submission>(
          host_error_code::unavailable, "runtime is not accepting new runs", true);
      }
      if (background_runs_.contains(run_id)) {
        return host_failure<agent::host::run_submission>(
          host_error_code::conflict, "agent run is already being submitted");
      }
      std::shared_ptr<std::stop_source> source;
      try {
        source = reserve_run(run_id);
        future = std::async(std::launch::async,
          [this, prompt = std::move(prompt), context = std::move(context), source]() mutable {
            return execute_reserved_run(std::move(prompt), std::move(context), std::move(source));
          }).share();
      }
      catch (...) {
        if (source)
          release_run(run_id);
        throw;
      }
      background_runs_.emplace(run_id, future);
    }

    const auto startup_deadline =
      std::chrono::steady_clock::now() + config_->runtime.submission_timeout;
    while (std::chrono::steady_clock::now() < startup_deadline) {
      if (const auto durable = durable_runtime()) {
        if (const auto record = durable->get(run_id)) {
          return agent::host::host_result<agent::host::run_submission>::success({
            .run_id = record->id,
            .revision = record->revision,
            .status = record->status,
          });
        }
      }
      if (future.wait_for(std::chrono::milliseconds(1)) == std::future_status::ready) {
        try {
          (void)future.get();
        }
        catch (const std::invalid_argument& ex) {
          return host_failure<agent::host::run_submission>(
            host_error_code::invalid_request, ex.what());
        }
        catch (const runtime_error& ex) {
          const bool capacity = ex.code() == runtime_error_code::capacity_exceeded;
          return host_failure<agent::host::run_submission>(host_error_code::unavailable,
            capacity ? ex.what() : "runtime could not execute the run",
            capacity);
        }
        catch (...) {
          return host_failure<agent::host::run_submission>(
            host_error_code::internal, "runtime could not execute the run");
        }
      }
    }
    (void)cancel_run(run_id);
    return host_failure<agent::host::run_submission>(
      host_error_code::unavailable, "agent run did not start before the submission deadline", true);
  }

  agent::host::host_result<agent::host::run_submission> submit_background_resume(
    agent::runtime::agent_run_record record, agent::runtime::agent_run_suspension continuation,
    const agent::host::resume_run_request& request,
    std::shared_ptr<agent::runtime::agent_run_runtime> durable) {
    ensure_ready();
    reap_background_runs();
    const auto runner = agent_runner();
    if (!runner) {
      return host_failure<agent::host::run_submission>(
        host_error_code::unavailable, "runtime execution dependencies are unavailable", true);
    }

    std::shared_future<run_result> future;
    {
      std::scoped_lock lock(background_mutex_);
      if (lifecycle_.state() != lifecycle_state::ready) {
        return host_failure<agent::host::run_submission>(
          host_error_code::unavailable, "runtime is not accepting resumed runs", true);
      }
      if (background_runs_.contains(request.run_id)) {
        return host_failure<agent::host::run_submission>(
          host_error_code::conflict, "agent run is already active");
      }
      const auto run_id = request.run_id;
      const auto expected_revision = request.expected_revision;
      std::shared_ptr<std::stop_source> source;
      try {
        source = reserve_run(run_id);
        future = std::async(std::launch::async,
          [this,
            record = std::move(record),
            continuation = std::move(continuation),
            durable,
            runner,
            source,
            run_id,
            expected_revision]() mutable {
            struct release_guard {
              implementation& owner;
              std::string run_id;
              ~release_guard() {
                owner.release_run(run_id);
              }
            } release { *this, run_id };
            try {
              llm_agent_run_options options {
                .stop_token = source->get_token(),
                .context = std::move(record.context),
                .runtime = durable,
              };
              const auto response = runner->resume(
                run_id, expected_revision, continuation.continuation_token, std::move(options));
              const auto updated = durable->get(run_id);
              if (!updated) {
                throw runtime_error(runtime_error_code::execution_failed,
                  "resumed agent run disappeared from storage");
              }
              return run_result {
                .run_id = updated->id,
                .revision = updated->revision,
                .status = updated->status,
                .content = response.content,
                .error = updated->error,
                .result = updated->result,
                .metadata = response.metadata,
              };
            }
            catch (const std::exception& ex) {
              fail_running_run(run_id, ex.what());
              throw;
            }
            catch (...) {
              fail_running_run(run_id, "resumed runtime execution failed");
              throw;
            }
          }).share();
      }
      catch (...) {
        if (source)
          release_run(run_id);
        throw;
      }
      background_runs_.emplace(run_id, future);
    }

    const auto startup_deadline =
      std::chrono::steady_clock::now() + config_->runtime.submission_timeout;
    while (std::chrono::steady_clock::now() < startup_deadline) {
      if (const auto updated = durable->get(request.run_id);
          updated && updated->revision > request.expected_revision) {
        return agent::host::host_result<agent::host::run_submission>::success({
          .run_id = updated->id,
          .revision = updated->revision,
          .status = updated->status,
        });
      }
      if (future.wait_for(std::chrono::milliseconds(1)) == std::future_status::ready) {
        try {
          const auto result = future.get();
          return agent::host::host_result<agent::host::run_submission>::success({
            .run_id = result.run_id,
            .revision = result.revision,
            .status = result.status,
          });
        }
        catch (const std::invalid_argument& ex) {
          return host_failure<agent::host::run_submission>(
            host_error_code::invalid_request, ex.what());
        }
        catch (const std::logic_error& ex) {
          return host_failure<agent::host::run_submission>(host_error_code::conflict, ex.what());
        }
        catch (...) {
          return host_failure<agent::host::run_submission>(
            host_error_code::internal, "agent run could not be resumed");
        }
      }
    }
    (void)cancel_run(request.run_id);
    return host_failure<agent::host::run_submission>(host_error_code::unavailable,
      "agent run did not resume before the submission deadline",
      true);
  }

  void reap_background_runs() const {
    std::scoped_lock lock(background_mutex_);
    for (auto current = background_runs_.begin(); current != background_runs_.end();) {
      if (current->second.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
        current = background_runs_.erase(current);
      else
        ++current;
    }
  }

  std::shared_ptr<std::stop_source> reserve_run(const std::string& run_id) {
    std::scoped_lock lock(active_mutex_);
    if (lifecycle_.state() != lifecycle_state::ready) {
      throw runtime_error(runtime_error_code::invalid_state, "runtime is not accepting new runs");
    }
    if (active_runs_.contains(run_id)) {
      throw runtime_error(
        runtime_error_code::run_conflict, "agent run is already active: " + run_id);
    }
    if (active_runs_.size() >= config_->runtime.max_concurrent_runs) {
      throw runtime_error(
        runtime_error_code::capacity_exceeded, "runtime concurrent run capacity is exhausted");
    }
    auto source = std::make_shared<std::stop_source>();
    active_runs_.emplace(run_id, source);
    return source;
  }

  void release_run(const std::string& run_id) noexcept {
    {
      std::scoped_lock lock(active_mutex_);
      active_runs_.erase(run_id);
    }
    active_changed_.notify_all();
  }

  void fail_incomplete_run(const std::string& run_id, std::string message) noexcept {
    try {
      const auto durable = durable_runtime();
      const auto record = durable ? durable->get(run_id) : std::nullopt;
      if (record && (record->status == agent_run_status::created ||
                      record->status == agent_run_status::running)) {
        (void)durable->finish(run_id,
          record->revision,
          agent_run_status::failed,
          nlohmann::json::object(),
          std::move(message),
          "run_failed");
      }
    }
    catch (...) {
    }
  }

  void fail_running_run(const std::string& run_id, std::string message) noexcept {
    try {
      const auto durable = durable_runtime();
      const auto record = durable ? durable->get(run_id) : std::nullopt;
      if (record && record->status == agent_run_status::running) {
        (void)durable->finish(run_id,
          record->revision,
          agent_run_status::failed,
          nlohmann::json::object(),
          std::move(message),
          "run_failed");
      }
    }
    catch (...) {
    }
  }

  void shutdown_for_destruction() noexcept {
    try {
      {
        std::unique_lock lock(active_mutex_);
        for (const auto& [_, source] : active_runs_)
          source->request_stop();
        active_changed_.wait(lock, [&] { return active_runs_.empty(); });
      }
      {
        std::scoped_lock background_lock(background_mutex_);
        background_runs_.clear();
      }
      {
        std::scoped_lock resources_lock(resources_mutex_);
        runner_.reset();
        client_.reset();
        idempotency_repository_.reset();
        durable_runtime_.reset();
        store_.reset();
      }
    }
    catch (...) {
    }
  }

  std::shared_ptr<agent::runtime::agent_run_runtime> durable_runtime() const {
    std::scoped_lock lock(resources_mutex_);
    return durable_runtime_;
  }

  std::shared_ptr<llm_agent_runner> agent_runner() const {
    std::scoped_lock lock(resources_mutex_);
    return runner_;
  }

  std::shared_ptr<detail::idempotency_repository> idempotency_repository() const {
    std::scoped_lock lock(resources_mutex_);
    return idempotency_repository_;
  }

  bool uses_default_run_store_ { true };
  runtime_dependencies dependencies_;
  std::optional<runtime_config> config_;
  std::mutex control_mutex_;
  lifecycle lifecycle_;
  mutable std::mutex resources_mutex_;
  std::shared_ptr<agent::runtime::agent_run_store> store_;
  std::shared_ptr<agent::runtime::agent_run_runtime> durable_runtime_;
  std::shared_ptr<detail::idempotency_repository> idempotency_repository_;
  std::shared_ptr<llm_client> client_;
  std::shared_ptr<llm_agent_runner> runner_;
  mutable std::mutex active_mutex_;
  std::condition_variable active_changed_;
  std::unordered_map<std::string, std::shared_ptr<std::stop_source>> active_runs_;
  mutable std::mutex background_mutex_;
  mutable std::unordered_map<std::string, std::shared_future<run_result>> background_runs_;
  static constexpr std::size_t max_idempotency_entries = 4096;
  std::mutex idempotency_mutex_;
  std::unordered_map<std::string, std::shared_ptr<idempotency_entry>> idempotency_;
};

host::host(runtime_dependencies dependencies)
    : impl_(std::make_unique<implementation>(std::move(dependencies))) {
}

host::~host() = default;

void host::configure(runtime_config config) {
  impl_->configure(std::move(config));
}

void host::start() {
  impl_->start();
}

bool host::stop(std::optional<std::chrono::milliseconds> timeout) noexcept {
  return impl_->stop(timeout);
}

lifecycle_snapshot host::lifecycle_status() const {
  return impl_->lifecycle_status();
}

health_snapshot host::health() const {
  return impl_->health();
}

const runtime_config& host::config() const {
  return impl_->config();
}

run_result host::run(std::string prompt, agent::core::agent_execution_context context) {
  return impl_->run(std::move(prompt), std::move(context));
}

bool host::cancel_run(const std::string& run_id) {
  return impl_->cancel_run(run_id);
}

std::optional<agent::host::run_view> host::get_run(const std::string& run_id) const {
  return impl_->get_run_view(run_id);
}

nlohmann::json host::dispatch_protocol_request(const nlohmann::json& request) {
  return impl_->dispatch(request);
}

agent::host::agent_host_service& host::protocol_service() {
  return *impl_;
}

nlohmann::json health_to_json(const health_snapshot& health) {
  nlohmann::json checks = nlohmann::json::object();
  for (const auto& [name, check] : health.checks)
    checks[name] = { { "ok", check.ok }, { "message", check.message } };
  return {
    { "status", to_string(health.status) },
    { "lifecycle", to_string(health.lifecycle.state) },
    { "failure", health.lifecycle.failure },
    { "checks", std::move(checks) },
  };
}

nlohmann::json doctor_report_to_json(const doctor_report& report) {
  nlohmann::json checks = nlohmann::json::object();
  for (const auto& [name, check] : report.checks)
    checks[name] = { { "ok", check.ok }, { "message", check.message } };
  return { { "ready", report.ready }, { "checks", std::move(checks) } };
}

nlohmann::json run_result_to_json(const run_result& result) {
  return {
    { "runId", result.run_id },
    { "revision", result.revision },
    { "status", agent::runtime::to_string(result.status) },
    { "content", result.content },
    { "error", result.error },
    { "result", result.result },
    { "metadata", result.metadata },
  };
}

} // namespace wuwe::runtime_host
