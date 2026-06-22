#ifndef WUWE_AGENT_EXECUTION_EXECUTION_REGISTRY_HPP
#define WUWE_AGENT_EXECUTION_EXECUTION_REGISTRY_HPP

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <wuwe/agent/execution/execution_backend.hpp>

namespace wuwe::agent::execution {

struct execution_backend_requirements {
  std::optional<sandbox::isolation_level> isolation;
  bool require_shell_disabled { false };
  bool require_timeout { false };
  bool require_cancellation { false };
  bool require_stdout_limit { false };
  bool require_stderr_limit { false };
  bool require_environment_allowlist { false };
  bool require_process_tree_cleanup { false };
  bool require_process_count_limit { false };
  bool require_cpu_time_limit { false };
  bool require_memory_limit { false };
  bool require_filesystem_read_deny { false };
  bool require_filesystem_write_deny { false };
  bool require_network_deny { false };
};

class execution_backend_registry {
public:
  using factory = std::function<std::unique_ptr<execution_backend>()>;

  void register_backend(std::string name, factory create);
  void register_descriptor(sandbox::sandbox_backend_info info);

  [[nodiscard]] std::unique_ptr<execution_backend> create(
    const std::string& name) const;

  [[nodiscard]] std::optional<sandbox::sandbox_backend_info> describe(
    const std::string& name) const;

  [[nodiscard]] std::vector<sandbox::sandbox_backend_info> backends() const;

  [[nodiscard]] std::optional<std::string> select_backend_name(
    const execution_backend_requirements& requirements) const;

  [[nodiscard]] std::unique_ptr<execution_backend> create_best(
    const execution_backend_requirements& requirements) const;

private:
  struct entry {
    std::string name;
    factory create;
    std::optional<sandbox::sandbox_backend_info> descriptor;
  };

  std::vector<entry> entries_;
};

[[nodiscard]] execution_backend_registry make_default_execution_backend_registry();

} // namespace wuwe::agent::execution

#endif // WUWE_AGENT_EXECUTION_EXECUTION_REGISTRY_HPP
