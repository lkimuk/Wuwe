#include <wuwe/runtime_host/validation.hpp>

void wuwe_runtime_validation_header_is_self_contained() {
  const wuwe::runtime_host::validation_report report;
  (void)report.valid();
}
