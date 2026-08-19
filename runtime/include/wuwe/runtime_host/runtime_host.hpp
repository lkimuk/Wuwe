#ifndef WUWE_RUNTIME_HOST_RUNTIME_HOST_HPP
#define WUWE_RUNTIME_HOST_RUNTIME_HOST_HPP

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include <wuwe/agent/core/execution_context.hpp>
#include <wuwe/agent/host/host_service.hpp>
#include <wuwe/runtime_host/config.hpp>
#include <wuwe/runtime_host/lifecycle.hpp>

namespace wuwe {

class llm_agent_runner;
class llm_client;

namespace agent::runtime {

class agent_run_store;

} // namespace agent::runtime

namespace runtime_host {

enum class runtime_error_code {
  invalid_state,
  dependency_unavailable,
  capacity_exceeded,
  run_conflict,
  execution_failed,
};

[[nodiscard]] std::string to_string(runtime_error_code code);

class runtime_error final : public std::runtime_error {
public:
  runtime_error(runtime_error_code code, std::string message);

  [[nodiscard]] runtime_error_code code() const noexcept {
    return code_;
  }

private:
  runtime_error_code code_;
};

enum class health_status {
  healthy,
  degraded,
  unhealthy,
};

[[nodiscard]] std::string to_string(health_status status);

struct health_check {
  bool ok { false };
  std::string message;
};

struct health_snapshot {
  health_status status { health_status::unhealthy };
  lifecycle_snapshot lifecycle;
  std::map<std::string, health_check> checks;
};

struct run_result {
  std::string run_id;
  std::uint64_t revision { 0 };
  agent::runtime::agent_run_status status { agent::runtime::agent_run_status::failed };
  std::string content;
  std::string error;
  nlohmann::json result = nlohmann::json::object();
  std::map<std::string, std::string> metadata;
};

struct runtime_dependencies {
  // Factories run during start(). Returning an empty pointer fails startup. The host retains the
  // client for at least as long as the runner produced from it.
  std::function<std::shared_ptr<llm_client>(const llm_options&)> make_llm_client;
  std::function<std::shared_ptr<llm_agent_runner>(llm_client&)> make_agent_runner;
  std::function<std::shared_ptr<agent::runtime::agent_run_store>(const run_store_options&)>
    make_run_store;
};

struct doctor_report {
  bool ready { false };
  std::map<std::string, health_check> checks;
};

// Performs side-effect-free preflight checks. Custom factories declare responsibility for their
// corresponding dependency; host::start() remains the authoritative assembly check.
[[nodiscard]] doctor_report diagnose_runtime_config(
  const runtime_config& config, const runtime_dependencies& dependencies = {});

class host final {
public:
  explicit host(runtime_dependencies dependencies = {});
  ~host();

  host(const host&) = delete;
  host& operator=(const host&) = delete;
  host(host&&) = delete;
  host& operator=(host&&) = delete;

  // Lifecycle operations are serialized. configure() and start() are single-use; stop() is
  // idempotent after a clean shutdown and may run while executions are active.
  void configure(runtime_config config);
  void start();
  // Returns false when graceful shutdown times out or the host has failed. Destruction still waits
  // for active in-process calls to finish so their references to the host remain valid.
  [[nodiscard]] bool stop(std::optional<std::chrono::milliseconds> timeout = std::nullopt) noexcept;

  [[nodiscard]] lifecycle_snapshot lifecycle_status() const;
  [[nodiscard]] health_snapshot health() const;
  [[nodiscard]] const runtime_config& config() const;

  [[nodiscard]] run_result run(
    std::string prompt, agent::core::agent_execution_context context = {});
  // Requests cancellation of an active run. False means the run is not active or was already
  // cancellation-requested; durable state remains available through get_run().
  [[nodiscard]] bool cancel_run(const std::string& run_id);

  [[nodiscard]] std::optional<agent::host::run_view> get_run(const std::string& run_id) const;
  // JSON and typed entry points for Agent Host Protocol transports. Returned service references are
  // owned by the host and remain valid until host destruction.
  [[nodiscard]] nlohmann::json dispatch_protocol_request(const nlohmann::json& request);
  [[nodiscard]] agent::host::agent_host_service& protocol_service();

private:
  class implementation;
  std::unique_ptr<implementation> impl_;
};

[[nodiscard]] nlohmann::json health_to_json(const health_snapshot& health);
[[nodiscard]] nlohmann::json doctor_report_to_json(const doctor_report& report);
[[nodiscard]] nlohmann::json run_result_to_json(const run_result& result);

} // namespace runtime_host
} // namespace wuwe

#endif // WUWE_RUNTIME_HOST_RUNTIME_HOST_HPP
