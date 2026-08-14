#include <wuwe/agent/training/training_context.hpp>

bool training_context_header_is_independent() {
  return !wuwe::agent::training::training_context {}.cancellation_requested();
}
