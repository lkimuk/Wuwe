#include <wuwe/runtime_host/lifecycle.hpp>

bool wuwe_runtime_lifecycle_header_probe() {
  return wuwe::runtime_host::terminal(wuwe::runtime_host::lifecycle_state::stopped);
}
