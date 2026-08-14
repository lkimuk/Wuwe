#include <wuwe/agent/training/training_core.hpp>

bool training_core_header_is_independent() {
  return wuwe::agent::training::terminal(wuwe::agent::training::training_job_status::succeeded);
}
