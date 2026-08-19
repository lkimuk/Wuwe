#include <wuwe/runtime_host.hpp>
#include <wuwe/version.hpp>

int main() {
  wuwe::runtime_host::host host;
  const auto state = host.lifecycle_status();
  return state.state == wuwe::runtime_host::lifecycle_state::created &&
             wuwe::framework_version_major == 1
           ? 0
           : 1;
}
