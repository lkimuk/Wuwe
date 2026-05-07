#include <wuwe/agent/mcp/mcp_host_runtime.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace wuwe::agent::mcp {
namespace {

std::filesystem::path env_path(const char* name) {
#ifdef _WIN32
  char* value = nullptr;
  std::size_t size = 0;
  if (_dupenv_s(&value, &size, name) != 0 || !value) {
    return {};
  }
  std::filesystem::path path = *value ? std::filesystem::path(value) : std::filesystem::path {};
  std::free(value);
  return path;
#else
  const auto* value = std::getenv(name);
  return value && *value ? std::filesystem::path(value) : std::filesystem::path {};
#endif
}

std::filesystem::path resolve_home_path(const std::filesystem::path& home) {
  if (!home.empty()) {
    return home;
  }
  const auto user_profile = env_path("USERPROFILE");
  return user_profile.empty() ? env_path("HOME") : user_profile;
}

void push_unique(
  std::vector<std::filesystem::path>& paths,
  const std::filesystem::path& path) {
  if (path.empty()) {
    return;
  }
  if (std::find(paths.begin(), paths.end(), path) == paths.end()) {
    paths.push_back(path);
  }
}

std::chrono::milliseconds optional_milliseconds(
  const json& value,
  std::string_view key,
  std::chrono::milliseconds fallback,
  const std::string& server_id) {
  if (!value.contains(key)) {
    return fallback;
  }
  if (!value.at(key).is_number_integer()) {
    throw std::runtime_error("MCP host server " + std::string(key) +
                             " must be an integer: " + server_id);
  }
  const auto count = value.at(key).get<long long>();
  if (count < 0) {
    throw std::runtime_error("MCP host server " + std::string(key) +
                             " must not be negative: " + server_id);
  }
  return std::chrono::milliseconds { count };
}

std::chrono::milliseconds remaining_until(std::chrono::steady_clock::time_point deadline) {
  if (deadline == std::chrono::steady_clock::time_point {}) {
    return std::chrono::milliseconds { 0 };
  }
  if (deadline == std::chrono::steady_clock::time_point::max()) {
    return std::chrono::milliseconds::max();
  }
  const auto now = std::chrono::steady_clock::now();
  if (deadline <= now) {
    return std::chrono::milliseconds { 0 };
  }
  return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
}

} // namespace

void mcp_host_runtime::set_event_sink(event_sink sink) {
  std::lock_guard lock(events_mutex_);
  event_sink_ = std::move(sink);
}

std::vector<mcp_host_event> mcp_host_runtime::events() const {
  std::lock_guard lock(events_mutex_);
  return events_;
}

void mcp_host_runtime::clear_events() {
  std::lock_guard lock(events_mutex_);
  events_.clear();
}

void mcp_host_runtime::add_server(mcp_host_server_config config) {
  if (config.id.empty()) {
    throw std::runtime_error("MCP host server id must not be empty");
  }
  if (config.command.command.empty()) {
    throw std::runtime_error("MCP host server command must not be empty: " + config.id);
  }
  if (servers_.find(config.id) != servers_.end()) {
    throw std::runtime_error("MCP host server already exists: " + config.id);
  }

  const auto id = config.id;
  auto [it, inserted] = servers_.emplace(id, server_entry { .config = std::move(config) });
  (void)inserted;
  record_event(mcp_host_event_type::server_added, it->second);
}

void mcp_host_runtime::add_servers_from_json(const json& config_json) {
  for (auto config : mcp_host_server_configs_from_json(config_json)) {
    add_server(std::move(config));
  }
}

std::size_t mcp_host_runtime::add_servers_from_file(const std::filesystem::path& path) {
  auto configs = mcp_host_server_configs_from_file(path);
  const auto count = configs.size();
  for (auto& config : configs) {
    add_server(std::move(config));
  }
  return count;
}

bool mcp_host_runtime::remove_server(const std::string& id) {
  auto it = servers_.find(id);
  if (it == servers_.end()) {
    return false;
  }
  record_event(mcp_host_event_type::server_removed, it->second);
  it->second.client.stop();
  servers_.erase(it);
  return true;
}

bool mcp_host_runtime::contains(const std::string& id) const {
  return servers_.find(id) != servers_.end();
}

void mcp_host_runtime::start_server(const std::string& id) {
  auto& entry = entry_for(id);
  std::lock_guard lock(*entry.mutex);
  start_entry(entry);
}

void mcp_host_runtime::restart_server(const std::string& id) {
  auto& entry = entry_for(id);
  std::lock_guard lock(*entry.mutex);
  restart_entry(entry);
}

void mcp_host_runtime::start_entry(server_entry& entry) {
  if (entry.client.running()) {
    entry.state = mcp_host_server_state::running;
    entry.error.clear();
    return;
  }

  entry.state = mcp_host_server_state::starting;
  entry.error.clear();
  record_event(mcp_host_event_type::server_starting, entry);
  try {
    entry.client.start(entry.config.command);
    if (entry.config.auto_initialize) {
      entry.client.initialize(entry.config.client_info, entry.config.capabilities);
      if (entry.config.send_initialized_notification) {
        entry.client.notify("notifications/initialized");
      }
    }
    entry.state = mcp_host_server_state::running;
    reset_failure_policy(entry);
    record_event(mcp_host_event_type::server_started, entry);
  }
  catch (const std::exception& ex) {
    entry.client.stop();
    record_failure(entry, ex.what());
    throw;
  }
}

bool mcp_host_runtime::can_restart(const server_entry& entry) const {
  return entry.config.restart_on_failure &&
         entry.restart_count < entry.config.max_restart_attempts &&
         restart_block_reason(entry).empty();
}

std::string mcp_host_runtime::restart_block_reason(const server_entry& entry) const {
  const auto now = std::chrono::steady_clock::now();
  if (entry.circuit_open_until != std::chrono::steady_clock::time_point {} &&
      entry.circuit_open_until > now) {
    return "MCP host server circuit breaker is open: " + entry.config.id;
  }
  if (entry.next_restart_at != std::chrono::steady_clock::time_point {} &&
      entry.next_restart_at > now) {
    return "MCP host server restart backoff is active: " + entry.config.id;
  }
  return {};
}

void mcp_host_runtime::record_failure(server_entry& entry, std::string error) {
  ++entry.failure_count;
  ++entry.consecutive_failure_count;
  entry.state = mcp_host_server_state::failed;
  entry.error = std::move(error);
  record_event(mcp_host_event_type::failure_recorded, entry, {}, entry.error);

  const auto delay = next_backoff_delay(entry);
  entry.next_restart_at = delay.count() > 0
      ? std::chrono::steady_clock::now() + delay
      : std::chrono::steady_clock::time_point {};

  if (entry.config.circuit_breaker_failure_threshold > 0 &&
      entry.consecutive_failure_count >= entry.config.circuit_breaker_failure_threshold) {
    const auto cooldown = entry.config.circuit_breaker_cooldown;
    entry.state = mcp_host_server_state::circuit_open;
    entry.circuit_open_until = cooldown.count() > 0
        ? std::chrono::steady_clock::now() + cooldown
        : std::chrono::steady_clock::time_point::max();
    record_event(mcp_host_event_type::circuit_opened, entry, {}, entry.error, {}, {
      { "failureThreshold", entry.config.circuit_breaker_failure_threshold },
      { "cooldownMillis", cooldown.count() },
    });
  }
}

void mcp_host_runtime::reset_failure_policy(server_entry& entry) {
  entry.consecutive_failure_count = 0;
  entry.next_restart_at = {};
  entry.circuit_open_until = {};
}

std::chrono::milliseconds mcp_host_runtime::next_backoff_delay(const server_entry& entry) const {
  const auto initial = entry.config.restart_backoff;
  if (initial.count() <= 0) {
    return std::chrono::milliseconds { 0 };
  }

  auto delay = initial;
  for (std::size_t i = 1; i < entry.consecutive_failure_count; ++i) {
    if (delay.count() > (std::numeric_limits<long long>::max)() / 2) {
      break;
    }
    delay *= 2;
  }
  const auto max_delay = entry.config.max_restart_backoff;
  if (max_delay.count() > 0 && delay > max_delay) {
    return max_delay;
  }
  return delay;
}

void mcp_host_runtime::restart_entry(server_entry& entry) {
  if (!can_restart(entry)) {
    const auto reason = restart_block_reason(entry);
    record_event(mcp_host_event_type::restart_blocked, entry, {}, reason);
    throw std::runtime_error(reason.empty()
      ? "MCP host server restart limit reached: " + entry.config.id
      : reason);
  }
  record_event(mcp_host_event_type::restart_started, entry);
  entry.client.stop();
  ++entry.restart_count;
  start_entry(entry);
}

void mcp_host_runtime::stop_server(const std::string& id) {
  auto& entry = entry_for(id);
  std::lock_guard lock(*entry.mutex);
  entry.client.stop();
  entry.state = mcp_host_server_state::stopped;
  entry.error.clear();
  entry.restart_count = 0;
  reset_failure_policy(entry);
  record_event(mcp_host_event_type::server_stopped, entry);
}

void mcp_host_runtime::start_all() {
  for (auto& [id, entry] : servers_) {
    (void)id;
    if (!entry.client.running()) {
      start_server(entry.config.id);
    }
  }
}

void mcp_host_runtime::stop_all() {
  for (auto& [id, entry] : servers_) {
    (void)id;
    std::lock_guard lock(*entry.mutex);
    entry.client.stop();
    entry.state = mcp_host_server_state::stopped;
    entry.error.clear();
    entry.restart_count = 0;
    reset_failure_policy(entry);
    record_event(mcp_host_event_type::server_stopped, entry);
  }
}

mcp_process_client& mcp_host_runtime::client(const std::string& id) {
  return entry_for(id).client;
}

const mcp_process_client& mcp_host_runtime::client(const std::string& id) const {
  return entry_for(id).client;
}

json mcp_host_runtime::request(const std::string& id, std::string method, json params) {
  auto& entry = entry_for(id);
  std::lock_guard lock(*entry.mutex);
  const auto started_at = std::chrono::steady_clock::now();
  record_event(mcp_host_event_type::request_started, entry, method);
  try {
    ++entry.request_count;
    const auto response = entry.client.request(method, params);
    entry.state = entry.client.running() ? mcp_host_server_state::running
                                         : mcp_host_server_state::stopped;
    entry.error.clear();
    if (entry.state == mcp_host_server_state::running) {
      reset_failure_policy(entry);
    }
    record_event(mcp_host_event_type::request_succeeded, entry, method, {},
      std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started_at));
    return response;
  }
  catch (const std::exception& ex) {
    record_failure(entry, ex.what());
    if (can_restart(entry)) {
      try {
        restart_entry(entry);
        const auto response = entry.client.request(method, params);
        entry.state = entry.client.running() ? mcp_host_server_state::running
                                             : mcp_host_server_state::stopped;
        entry.error.clear();
        reset_failure_policy(entry);
        record_event(mcp_host_event_type::request_succeeded, entry, method, {},
          std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_at),
          { { "retried", true } });
        return response;
      }
      catch (const std::exception& retry_ex) {
        record_failure(entry, retry_ex.what());
        record_event(mcp_host_event_type::request_failed, entry, method, retry_ex.what(),
          std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_at),
          { { "retried", true } });
        throw;
      }
    }
    record_event(mcp_host_event_type::request_failed, entry, method, ex.what(),
      std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started_at));
    throw;
  }
}

std::future<json> mcp_host_runtime::request_async(
  std::string id,
  std::string method,
  json params) {
  return std::async(
    std::launch::async,
    [this, id = std::move(id), method = std::move(method), params = std::move(params)]() mutable {
      return request(id, std::move(method), std::move(params));
    });
}

void mcp_host_runtime::notify(const std::string& id, std::string method, json params) {
  auto& entry = entry_for(id);
  std::lock_guard lock(*entry.mutex);
  const auto started_at = std::chrono::steady_clock::now();
  try {
    ++entry.request_count;
    entry.client.notify(std::move(method), std::move(params));
    entry.state = entry.client.running() ? mcp_host_server_state::running
                                         : mcp_host_server_state::stopped;
    entry.error.clear();
    if (entry.state == mcp_host_server_state::running) {
      reset_failure_policy(entry);
    }
    record_event(mcp_host_event_type::notification_sent, entry, method, {},
      std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started_at));
  }
  catch (const std::exception& ex) {
    record_failure(entry, ex.what());
    if (can_restart(entry)) {
      restart_entry(entry);
    }
    throw;
  }
}

json mcp_host_runtime::ping(const std::string& id) {
  return request(id, "ping");
}

json mcp_host_runtime::list_tools(const std::string& id, json params) {
  return request(id, "tools/list", std::move(params));
}

std::future<json> mcp_host_runtime::list_tools_async(std::string id, json params) {
  return request_async(std::move(id), "tools/list", std::move(params));
}

json mcp_host_runtime::call_tool(const std::string& id, std::string name, json arguments) {
  return request(id, "tools/call", {
    { "name", std::move(name) },
    { "arguments", std::move(arguments) },
  });
}

std::future<json> mcp_host_runtime::call_tool_async(
  std::string id,
  std::string name,
  json arguments) {
  return request_async(std::move(id), "tools/call", {
    { "name", std::move(name) },
    { "arguments", std::move(arguments) },
  });
}

json mcp_host_runtime::list_resources(const std::string& id, json params) {
  return request(id, "resources/list", std::move(params));
}

json mcp_host_runtime::read_resource(const std::string& id, std::string uri) {
  return request(id, "resources/read", { { "uri", std::move(uri) } });
}

json mcp_host_runtime::list_prompts(const std::string& id, json params) {
  return request(id, "prompts/list", std::move(params));
}

json mcp_host_runtime::get_prompt(const std::string& id, std::string name, json arguments) {
  return request(id, "prompts/get", {
    { "name", std::move(name) },
    { "arguments", std::move(arguments) },
  });
}

bool mcp_host_runtime::health_check(const std::string& id) {
  auto& entry = entry_for(id);
  std::lock_guard lock(*entry.mutex);
  const auto started_at = std::chrono::steady_clock::now();
  ++entry.health_check_count;
  record_event(mcp_host_event_type::health_check_started, entry, "ping");
  try {
    if (!entry.client.running()) {
      if (can_restart(entry)) {
        restart_entry(entry);
      }
      else {
        const auto reason = restart_block_reason(entry);
        if (!reason.empty()) {
          entry.error = reason;
          if (entry.circuit_open_until != std::chrono::steady_clock::time_point {} &&
              entry.circuit_open_until > std::chrono::steady_clock::now()) {
            entry.state = mcp_host_server_state::circuit_open;
          }
        }
        else {
          entry.state = mcp_host_server_state::stopped;
          entry.error = "server is not running";
        }
        return false;
      }
    }
    const auto response = entry.client.ping();
    const auto ok = response.contains("result") && response["result"].is_object();
    entry.state = ok ? mcp_host_server_state::running : mcp_host_server_state::failed;
    entry.error = ok ? std::string {} : std::string("ping returned non-success response");
    if (ok) {
      reset_failure_policy(entry);
      record_event(mcp_host_event_type::health_check_succeeded, entry, "ping", {},
        std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - started_at));
    }
    else {
      record_event(mcp_host_event_type::health_check_failed, entry, "ping", entry.error,
        std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - started_at));
    }
    return ok;
  }
  catch (const std::exception& ex) {
    record_failure(entry, ex.what());
    if (can_restart(entry)) {
      try {
        restart_entry(entry);
        record_event(mcp_host_event_type::health_check_succeeded, entry, "ping", {},
          std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_at),
          { { "restarted", true } });
        return true;
      }
      catch (const std::exception& retry_ex) {
        record_failure(entry, retry_ex.what());
      }
    }
    record_event(mcp_host_event_type::health_check_failed, entry, "ping", entry.error,
      std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started_at));
    return false;
  }
}

mcp_host_server_snapshot mcp_host_runtime::snapshot(const std::string& id) {
  auto& entry = entry_for(id);
  std::lock_guard lock(*entry.mutex);
  return snapshot_for(entry);
}

std::vector<mcp_host_server_snapshot> mcp_host_runtime::snapshots() {
  std::vector<mcp_host_server_snapshot> output;
  output.reserve(servers_.size());
  for (auto& [id, entry] : servers_) {
    (void)id;
    std::lock_guard lock(*entry.mutex);
    output.push_back(snapshot_for(entry));
  }
  return output;
}

mcp_host_runtime::server_entry& mcp_host_runtime::entry_for(const std::string& id) {
  auto it = servers_.find(id);
  if (it == servers_.end()) {
    throw std::runtime_error("MCP host server not found: " + id);
  }
  return it->second;
}

const mcp_host_runtime::server_entry& mcp_host_runtime::entry_for(const std::string& id) const {
  auto it = servers_.find(id);
  if (it == servers_.end()) {
    throw std::runtime_error("MCP host server not found: " + id);
  }
  return it->second;
}

mcp_host_server_snapshot mcp_host_runtime::snapshot_for(server_entry& entry) const {
  const auto running = entry.client.running();
  if (!running && entry.state == mcp_host_server_state::running) {
    entry.state = mcp_host_server_state::stopped;
  }
  const auto restart_backoff_remaining = remaining_until(entry.next_restart_at);
  const auto circuit_breaker_remaining = remaining_until(entry.circuit_open_until);
  if (entry.state == mcp_host_server_state::circuit_open &&
      circuit_breaker_remaining.count() == 0) {
    entry.state = mcp_host_server_state::failed;
  }
  const auto circuit_breaker_open = circuit_breaker_remaining.count() > 0 ||
                                    entry.circuit_open_until ==
                                      std::chrono::steady_clock::time_point::max();
  const auto restart_available = entry.config.restart_on_failure &&
                                 entry.restart_count < entry.config.max_restart_attempts &&
                                 restart_backoff_remaining.count() == 0 &&
                                 !circuit_breaker_open;

  return {
    .id = entry.config.id,
    .command = entry.config.command,
    .client_info = entry.config.client_info,
    .state = entry.state,
    .error = entry.error,
    .running = running,
    .restart_count = entry.restart_count,
    .request_count = entry.request_count,
    .failure_count = entry.failure_count,
    .consecutive_failure_count = entry.consecutive_failure_count,
    .health_check_count = entry.health_check_count,
    .restart_available = restart_available,
    .circuit_breaker_open = circuit_breaker_open,
    .restart_backoff_remaining = restart_backoff_remaining,
    .circuit_breaker_remaining = circuit_breaker_remaining,
    .stderr_output = entry.client.stderr_output(),
  };
}

void mcp_host_runtime::record_event(
  mcp_host_event_type type,
  const server_entry& entry,
  std::string method,
  std::string error,
  std::chrono::milliseconds elapsed,
  json metadata) {
  mcp_host_event event {
    .timestamp = std::chrono::system_clock::now(),
    .type = type,
    .server_id = entry.config.id,
    .method = std::move(method),
    .state = entry.state,
    .error = std::move(error),
    .elapsed = elapsed,
    .metadata = std::move(metadata),
  };

  event_sink sink;
  {
    std::lock_guard lock(events_mutex_);
    event.sequence = next_event_sequence_++;
    events_.push_back(event);
    sink = event_sink_;
  }
  if (sink) {
    sink(event);
  }
}

std::string to_string(mcp_host_server_state state) {
  switch (state) {
    case mcp_host_server_state::stopped:
      return "stopped";
    case mcp_host_server_state::starting:
      return "starting";
    case mcp_host_server_state::running:
      return "running";
    case mcp_host_server_state::failed:
      return "failed";
    case mcp_host_server_state::circuit_open:
      return "circuit_open";
  }
  return "unknown";
}

std::string to_string(mcp_host_event_type type) {
  switch (type) {
    case mcp_host_event_type::server_added:
      return "server_added";
    case mcp_host_event_type::server_removed:
      return "server_removed";
    case mcp_host_event_type::server_starting:
      return "server_starting";
    case mcp_host_event_type::server_started:
      return "server_started";
    case mcp_host_event_type::server_stopped:
      return "server_stopped";
    case mcp_host_event_type::request_started:
      return "request_started";
    case mcp_host_event_type::request_succeeded:
      return "request_succeeded";
    case mcp_host_event_type::request_failed:
      return "request_failed";
    case mcp_host_event_type::notification_sent:
      return "notification_sent";
    case mcp_host_event_type::health_check_started:
      return "health_check_started";
    case mcp_host_event_type::health_check_succeeded:
      return "health_check_succeeded";
    case mcp_host_event_type::health_check_failed:
      return "health_check_failed";
    case mcp_host_event_type::restart_started:
      return "restart_started";
    case mcp_host_event_type::restart_blocked:
      return "restart_blocked";
    case mcp_host_event_type::failure_recorded:
      return "failure_recorded";
    case mcp_host_event_type::circuit_opened:
      return "circuit_opened";
  }
  return "unknown";
}

std::vector<mcp_host_server_config> mcp_host_server_configs_from_json(const json& config_json) {
  if (!config_json.is_object()) {
    throw std::runtime_error("MCP host config must be a JSON object");
  }

  const json* servers = nullptr;
  if (config_json.contains("servers")) {
    servers = &config_json.at("servers");
  }
  else if (config_json.contains("mcpServers")) {
    servers = &config_json.at("mcpServers");
  }
  else {
    throw std::runtime_error("MCP host config must contain 'servers' or 'mcpServers'");
  }
  if (!servers->is_object()) {
    throw std::runtime_error("MCP host config servers must be an object");
  }

  std::vector<mcp_host_server_config> output;
  for (auto it = servers->begin(); it != servers->end(); ++it) {
    if (!it.value().is_object()) {
      throw std::runtime_error("MCP host server config must be an object: " + it.key());
    }
    const auto& value = it.value();
    const auto type = value.value("type", std::string("stdio"));
    if (type != "stdio") {
      throw std::runtime_error("MCP host runtime only supports stdio servers: " + it.key());
    }
    if (!value.contains("command") || !value["command"].is_string()) {
      throw std::runtime_error("MCP host server command must be a string: " + it.key());
    }

    mcp_host_server_config config;
    config.id = it.key();
    config.command.command = value["command"].get<std::string>();
    config.command.working_directory = value.value("cwd", value.value("workingDirectory", std::string()));
    if (value.contains("args")) {
      if (!value["args"].is_array()) {
        throw std::runtime_error("MCP host server args must be an array: " + it.key());
      }
      for (const auto& arg : value["args"]) {
        if (!arg.is_string()) {
          throw std::runtime_error("MCP host server args entries must be strings: " + it.key());
        }
        config.command.args.push_back(arg.get<std::string>());
      }
    }
    if (value.contains("env")) {
      if (!value["env"].is_object()) {
        throw std::runtime_error("MCP host server env must be an object: " + it.key());
      }
      for (auto env_it = value["env"].begin(); env_it != value["env"].end(); ++env_it) {
        if (!env_it.value().is_string()) {
          throw std::runtime_error("MCP host server env entries must be strings: " + it.key());
        }
        config.command.environment[env_it.key()] = env_it.value().get<std::string>();
      }
    }
    if (value.contains("clientInfo")) {
      if (!value["clientInfo"].is_object()) {
        throw std::runtime_error("MCP host server clientInfo must be an object: " + it.key());
      }
      config.client_info.name = value["clientInfo"].value("name", config.client_info.name);
      config.client_info.version = value["clientInfo"].value("version", config.client_info.version);
    }
    if (value.contains("capabilities")) {
      if (!value["capabilities"].is_object()) {
        throw std::runtime_error("MCP host server capabilities must be an object: " + it.key());
      }
      config.capabilities = value["capabilities"];
    }
    config.auto_initialize = value.value("autoInitialize", config.auto_initialize);
    config.send_initialized_notification =
      value.value("sendInitializedNotification", config.send_initialized_notification);
    config.restart_on_failure = value.value("restartOnFailure", config.restart_on_failure);
    config.max_restart_attempts = value.value("maxRestarts", config.max_restart_attempts);
    config.restart_backoff = optional_milliseconds(
      value, "restartBackoffMillis", config.restart_backoff, it.key());
    config.max_restart_backoff = optional_milliseconds(
      value, "maxRestartBackoffMillis", config.max_restart_backoff, it.key());
    config.circuit_breaker_failure_threshold =
      value.value("circuitBreakerFailureThreshold", config.circuit_breaker_failure_threshold);
    config.circuit_breaker_cooldown = optional_milliseconds(
      value, "circuitBreakerCooldownMillis", config.circuit_breaker_cooldown, it.key());
    output.push_back(std::move(config));
  }
  return output;
}

std::vector<mcp_host_server_config> mcp_host_server_configs_from_file(
  const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("failed to open MCP host config: " + path.string());
  }

  try {
    return mcp_host_server_configs_from_json(json::parse(input));
  }
  catch (const std::exception& ex) {
    throw std::runtime_error("failed to parse MCP host config " + path.string() + ": " + ex.what());
  }
}

std::vector<std::filesystem::path> mcp_host_user_config_paths(
  const std::filesystem::path& home,
  const std::filesystem::path& appdata) {
  std::vector<std::filesystem::path> paths;
  const auto resolved_home = resolve_home_path(home);
  const auto resolved_appdata = appdata.empty() ? env_path("APPDATA") : appdata;

#ifdef _WIN32
  if (!resolved_appdata.empty()) {
    push_unique(paths, resolved_appdata / "Claude" / "claude_desktop_config.json");
    push_unique(paths, resolved_appdata / "Code" / "User" / "mcp.json");
    push_unique(paths, resolved_appdata / "Cursor" / "User" / "mcp.json");
  }
  if (!resolved_home.empty()) {
    push_unique(paths, resolved_home / ".continue" / "config.json");
  }
#else
  if (!resolved_home.empty()) {
    push_unique(paths,
      resolved_home / "Library" / "Application Support" / "Claude" /
        "claude_desktop_config.json");
    push_unique(paths,
      resolved_home / "Library" / "Application Support" / "Code" / "User" / "mcp.json");
    push_unique(paths,
      resolved_home / "Library" / "Application Support" / "Cursor" / "User" / "mcp.json");
    push_unique(paths, resolved_home / ".continue" / "config.json");
  }
  const auto xdg_config_home = env_path("XDG_CONFIG_HOME");
  const auto config_home = !xdg_config_home.empty()
      ? xdg_config_home
      : resolved_home.empty() ? std::filesystem::path {} : resolved_home / ".config";
  if (!config_home.empty()) {
    push_unique(paths, config_home / "Code" / "User" / "mcp.json");
    push_unique(paths, config_home / "Cursor" / "User" / "mcp.json");
  }
#endif
  return paths;
}

std::vector<std::filesystem::path> mcp_host_default_config_paths(
  const std::filesystem::path& workspace_root) {
  std::vector<std::filesystem::path> paths;
  if (!workspace_root.empty()) {
    paths.push_back(workspace_root / ".vscode" / "mcp.json");
    paths.push_back(workspace_root / ".cursor" / "mcp.json");
  }
  return paths;
}

} // namespace wuwe::agent::mcp
