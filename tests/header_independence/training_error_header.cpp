#include <wuwe/agent/training/training_error.hpp>

bool training_error_header_is_independent() {
  return wuwe::agent::training::to_string(wuwe::agent::training::training_errc::not_found) ==
         "not_found";
}
