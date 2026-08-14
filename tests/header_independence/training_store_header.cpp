#include <wuwe/agent/training/training_store.hpp>

bool training_store_header_is_independent() {
  return wuwe::agent::training::in_memory_training_job_store {}.capabilities().declared;
}
