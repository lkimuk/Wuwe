#ifndef WUWE_AGENT_MCP_HOST_RUNTIME_HPP
#define WUWE_AGENT_MCP_HOST_RUNTIME_HPP

#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <wuwe/agent/mcp/mcp_process_client.hpp>

namespace wuwe::agent::mcp {

enum class mcp_host_server_state {
  stopped,
  starting,
  running,
  failed,
  circuit_open,
};

struct mcp_host_server_config {
  std::string id;
  mcp_process_command command;
  mcp_client_info client_info { .name = "wuwe-host", .version = "0.1.0" };
  json capabilities = json::object();
  bool auto_initialize { true };
  bool send_initialized_notification { true };
  bool restart_on_failure { false };
  std::size_t max_restart_attempts { 1 };
  std::chrono::milliseconds restart_backoff { 0 };
  std::chrono::milliseconds max_restart_backoff { 0 };
  std::size_t circuit_breaker_failure_threshold { 0 };
  std::chrono::milliseconds circuit_breaker_cooldown { 0 };
};

struct mcp_host_server_snapshot {
  std::string id;
  mcp_process_command command;
  mcp_client_info client_info;
  mcp_host_server_state state { mcp_host_server_state::stopped };
  std::string error;
  bool running { false };
  std::size_t restart_count { 0 };
  std::size_t request_count { 0 };
  std::size_t failure_count { 0 };
  std::size_t consecutive_failure_count { 0 };
  std::size_t health_check_count { 0 };
  bool restart_available { false };
  bool circuit_breaker_open { false };
  std::chrono::milliseconds restart_backoff_remaining { 0 };
  std::chrono::milliseconds circuit_breaker_remaining { 0 };
  std::string stderr_output;
};

enum class mcp_host_config_diagnostic_severity {
  error,
  warning,
};

struct mcp_host_config_diagnostic {
  mcp_host_config_diagnostic_severity severity { mcp_host_config_diagnostic_severity::error };
  std::string path;
  std::string message;
};

enum class mcp_host_event_type {
  server_added,
  server_removed,
  server_starting,
  server_started,
  server_stopped,
  request_started,
  request_succeeded,
  request_failed,
  notification_sent,
  health_check_started,
  health_check_succeeded,
  health_check_failed,
  restart_started,
  restart_blocked,
  failure_recorded,
  circuit_opened,
};

struct mcp_host_event {
  std::size_t sequence { 0 };
  std::chrono::system_clock::time_point timestamp;
  mcp_host_event_type type { mcp_host_event_type::server_added };
  std::string server_id;
  std::string method;
  mcp_host_server_state state { mcp_host_server_state::stopped };
  std::string error;
  std::chrono::milliseconds elapsed { 0 };
  json metadata = json::object();
};

class mcp_host_runtime {
public:
  using event_sink = std::function<void(const mcp_host_event&)>;

  void set_event_sink(event_sink sink);
  std::vector<mcp_host_event> events() const;
  void clear_events();

  void add_server(mcp_host_server_config config);
  void add_servers_from_json(const json& config_json);
  std::size_t add_servers_from_file(const std::filesystem::path& path);
  bool remove_server(const std::string& id);
  bool contains(const std::string& id) const;

  void start_server(const std::string& id);
  void restart_server(const std::string& id);
  void stop_server(const std::string& id);
  void start_all();
  void stop_all();

  mcp_process_client& client(const std::string& id);
  const mcp_process_client& client(const std::string& id) const;

  json request(const std::string& id, std::string method, json params = json::object());
  std::future<json> request_async(
    std::string id,
    std::string method,
    json params = json::object());
  void notify(const std::string& id, std::string method, json params = json::object());

  json ping(const std::string& id);
  json list_tools(const std::string& id, json params = json::object());
  std::future<json> list_tools_async(std::string id, json params = json::object());
  json call_tool(const std::string& id, std::string name, json arguments = json::object());
  std::future<json> call_tool_async(
    std::string id,
    std::string name,
    json arguments = json::object());
  json list_resources(const std::string& id, json params = json::object());
  json read_resource(const std::string& id, std::string uri);
  json list_prompts(const std::string& id, json params = json::object());
  json get_prompt(const std::string& id, std::string name, json arguments = json::object());
  bool health_check(const std::string& id);

  mcp_host_server_snapshot snapshot(const std::string& id);
  std::vector<mcp_host_server_snapshot> snapshots();

private:
  struct server_entry {
    mcp_host_server_config config;
    mcp_process_client client;
    mcp_host_server_state state { mcp_host_server_state::stopped };
    std::string error;
    std::size_t restart_count { 0 };
    std::size_t request_count { 0 };
    std::size_t failure_count { 0 };
    std::size_t consecutive_failure_count { 0 };
    std::size_t health_check_count { 0 };
    std::chrono::steady_clock::time_point next_restart_at {};
    std::chrono::steady_clock::time_point circuit_open_until {};
    std::shared_ptr<std::mutex> mutex { std::make_shared<std::mutex>() };
  };

  server_entry& entry_for(const std::string& id);
  const server_entry& entry_for(const std::string& id) const;
  void start_entry(server_entry& entry);
  bool can_restart(const server_entry& entry) const;
  std::string restart_block_reason(const server_entry& entry) const;
  void record_failure(server_entry& entry, std::string error);
  void reset_failure_policy(server_entry& entry);
  std::chrono::milliseconds next_backoff_delay(const server_entry& entry) const;
  void restart_entry(server_entry& entry);
  mcp_host_server_snapshot snapshot_for(server_entry& entry) const;
  void record_event(
    mcp_host_event_type type,
    const server_entry& entry,
    std::string method = {},
    std::string error = {},
    std::chrono::milliseconds elapsed = {},
    json metadata = json::object());

  std::map<std::string, server_entry> servers_;
  mutable std::mutex events_mutex_;
  event_sink event_sink_;
  std::vector<mcp_host_event> events_;
  std::size_t next_event_sequence_ { 1 };
};

std::string to_string(mcp_host_server_state state);
std::string to_string(mcp_host_config_diagnostic_severity severity);
std::string to_string(mcp_host_event_type type);
std::vector<mcp_host_server_config> mcp_host_server_configs_from_json(const json& config_json);
std::vector<mcp_host_server_config> mcp_host_server_configs_from_file(
  const std::filesystem::path& path);
std::vector<mcp_host_config_diagnostic> mcp_host_config_diagnostics_from_json(
  const json& config_json);
std::vector<mcp_host_config_diagnostic> mcp_host_config_diagnostics_from_file(
  const std::filesystem::path& path);
std::vector<std::filesystem::path> mcp_host_user_config_paths(
  const std::filesystem::path& home = {},
  const std::filesystem::path& appdata = {});
std::vector<std::filesystem::path> mcp_host_default_config_paths(
  const std::filesystem::path& workspace_root = {});

} // namespace wuwe::agent::mcp

#endif // WUWE_AGENT_MCP_HOST_RUNTIME_HPP
