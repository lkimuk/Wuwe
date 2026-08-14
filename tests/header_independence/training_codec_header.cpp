#include <wuwe/agent/training/training_codec.hpp>

bool training_codec_header_is_independent() {
  return wuwe::agent::training::training_time_from_json({ { "value", 0 } }, "value") ==
         std::chrono::system_clock::time_point {};
}
