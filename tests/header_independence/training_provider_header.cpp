#include <wuwe/agent/training/training_provider.hpp>

bool training_provider_header_is_independent() {
  return wuwe::agent::training::capability_name(
           wuwe::agent::training::training_operation::submit) != nullptr;
}
