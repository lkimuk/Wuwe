#include <wuwe/agent/training/training_json.hpp>

bool training_json_header_is_independent() {
  return wuwe::agent::training::training_example_to_json({}).is_object();
}
