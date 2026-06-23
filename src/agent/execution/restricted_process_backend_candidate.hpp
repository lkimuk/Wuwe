#ifndef WUWE_AGENT_EXECUTION_RESTRICTED_PROCESS_BACKEND_CANDIDATE_HPP
#define WUWE_AGENT_EXECUTION_RESTRICTED_PROCESS_BACKEND_CANDIDATE_HPP

#include <memory>

#include <wuwe/agent/execution/execution_backend.hpp>
#include <wuwe/agent/execution/restricted_process_backend.hpp>

namespace wuwe::agent::execution::detail {

[[nodiscard]] std::unique_ptr<execution_backend>
make_restricted_process_backend_candidate(
  restricted_process_backend_config config = {});

} // namespace wuwe::agent::execution::detail

#endif // WUWE_AGENT_EXECUTION_RESTRICTED_PROCESS_BACKEND_CANDIDATE_HPP
