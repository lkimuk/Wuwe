#ifndef WUWE_AGENT_KNOWLEDGE_TIKA_RUNTIME_HPP
#define WUWE_AGENT_KNOWLEDGE_TIKA_RUNTIME_HPP

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include <wuwe/net/http_client.h>

namespace wuwe::agent::knowledge {

struct tika_runtime_config {
  std::string host { "127.0.0.1" };
  int port { 9998 };
  std::string base_url;
  std::filesystem::path runtime_dir;
  std::filesystem::path jar_path;
  std::filesystem::path java_path;
  int startup_timeout_ms { 30000 };
  int poll_interval_ms { 250 };
  bool stop_on_destroy { true };
};

struct tika_runtime_discovery {
  bool found { false };
  tika_runtime_config config;
  std::string reason;
};

class tika_runtime_process {
public:
  tika_runtime_process();
  ~tika_runtime_process();

  tika_runtime_process(const tika_runtime_process&) = delete;
  tika_runtime_process& operator=(const tika_runtime_process&) = delete;

  tika_runtime_process(tika_runtime_process&&) noexcept;
  tika_runtime_process& operator=(tika_runtime_process&&) noexcept;

  static tika_runtime_discovery discover(tika_runtime_config config = {});
  static bool service_available(
    const std::string& base_url,
    int timeout_ms = 1000,
    std::shared_ptr<::wuwe::http_client> http = {});
  static std::shared_ptr<tika_runtime_process> ensure_running(tika_runtime_config config = {});

  void start(tika_runtime_config config = {});
  void stop();

  bool running() const;
  bool owns_process() const noexcept;
  const std::string& base_url() const noexcept;
  const tika_runtime_config& config() const noexcept;

private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

} // namespace wuwe::agent::knowledge

#endif // WUWE_AGENT_KNOWLEDGE_TIKA_RUNTIME_HPP
