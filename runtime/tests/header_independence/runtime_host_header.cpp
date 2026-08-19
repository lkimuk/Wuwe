#include <wuwe/runtime_host/runtime_host.hpp>

bool wuwe_runtime_host_header_probe() {
  wuwe::runtime_host::host host;
  return host.lifecycle_status().state == wuwe::runtime_host::lifecycle_state::created;
}
