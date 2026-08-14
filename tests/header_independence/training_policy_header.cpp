#include <wuwe/agent/training/training_policy.hpp>

bool training_policy_header_is_independent() {
  return wuwe::agent::training::trusted_training_policy().allow_submit;
}
