#include <wuwe/agent/training/http_training_provider.hpp>

bool http_training_provider_header_is_independent() {
  return !wuwe::agent::training::training_protocol_version.empty();
}
