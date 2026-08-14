#ifndef WUWE_AGENT_TRAINING_TRAINING_DATASET_HPP
#define WUWE_AGENT_TRAINING_TRAINING_DATASET_HPP

#include <stdexcept>
#include <string>

#include <wuwe/agent/training/training_json.hpp>

namespace wuwe::agent::training {

inline std::string export_training_jsonl(const training_dataset& value) {
  validate_training_dataset(value);
  return canonical_training_dataset_jsonl(value);
}

} // namespace wuwe::agent::training

#endif // WUWE_AGENT_TRAINING_TRAINING_DATASET_HPP
