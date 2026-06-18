#ifndef WUWE_AGENT_SANDBOX_SANDBOX_BACKEND_HPP
#define WUWE_AGENT_SANDBOX_SANDBOX_BACKEND_HPP

#include <wuwe/agent/sandbox/sandbox.hpp>

namespace wuwe::agent::sandbox {

class sandbox_backend {
public:
  virtual ~sandbox_backend() = default;

  [[nodiscard]] virtual sandbox_backend_info info() const = 0;
};

} // namespace wuwe::agent::sandbox

#endif // WUWE_AGENT_SANDBOX_SANDBOX_BACKEND_HPP
