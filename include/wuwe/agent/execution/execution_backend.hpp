#ifndef WUWE_AGENT_EXECUTION_EXECUTION_BACKEND_HPP
#define WUWE_AGENT_EXECUTION_EXECUTION_BACKEND_HPP

#include <stop_token>

#include <wuwe/agent/execution/execution_core.hpp>
#include <wuwe/agent/sandbox/sandbox.hpp>

namespace wuwe::agent::execution {

class execution_backend {
public:
  virtual ~execution_backend() = default;

  [[nodiscard]] virtual sandbox::sandbox_backend_info info() const = 0;

  [[nodiscard]] virtual execution_result run(
    const execution_request& request,
    std::stop_token stop_token) = 0;
};

} // namespace wuwe::agent::execution

#endif // WUWE_AGENT_EXECUTION_EXECUTION_BACKEND_HPP
