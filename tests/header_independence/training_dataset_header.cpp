#include <wuwe/agent/training/training_dataset.hpp>

bool training_dataset_header_is_independent() {
  return wuwe::agent::training::canonical_training_dataset_jsonl({}).empty();
}
