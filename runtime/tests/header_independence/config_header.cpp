#include <wuwe/runtime_host/config.hpp>

int wuwe_runtime_config_header_probe() {
  return wuwe::runtime_host::runtime_config_schema_version;
}
