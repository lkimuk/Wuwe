#ifndef WUWE_AGENT_EXECUTION_EXECUTION_REGISTRY_HPP
#define WUWE_AGENT_EXECUTION_EXECUTION_REGISTRY_HPP

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <wuwe/agent/execution/execution_backend.hpp>

namespace wuwe::agent::execution {

class execution_backend_registry {
public:
  using factory = std::function<std::unique_ptr<execution_backend>()>;

  void register_backend(std::string name, factory create);

  [[nodiscard]] std::unique_ptr<execution_backend> create(
    const std::string& name) const;

  [[nodiscard]] std::vector<sandbox::sandbox_backend_info> backends() const;

private:
  struct entry {
    std::string name;
    factory create;
  };

  std::vector<entry> entries_;
};

[[nodiscard]] execution_backend_registry make_default_execution_backend_registry();

} // namespace wuwe::agent::execution

#endif // WUWE_AGENT_EXECUTION_EXECUTION_REGISTRY_HPP
