#ifndef WUWE_AGENT_EXECUTION_EXECUTION_CODEC_HPP
#define WUWE_AGENT_EXECUTION_EXECUTION_CODEC_HPP

#include <nlohmann/json.hpp>

#include <wuwe/agent/execution/execution_core.hpp>

namespace wuwe::agent::execution {

[[nodiscard]] nlohmann::json execution_result_to_json(
  const execution_result& result);

} // namespace wuwe::agent::execution

#endif // WUWE_AGENT_EXECUTION_EXECUTION_CODEC_HPP
