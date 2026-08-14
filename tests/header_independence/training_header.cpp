#include <wuwe/agent/training/training.hpp>

bool training_header_is_independent() {
  return wuwe::agent::training::terminal(wuwe::agent::training::training_job_status::succeeded);
}
