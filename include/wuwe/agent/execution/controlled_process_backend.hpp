#ifndef WUWE_AGENT_EXECUTION_CONTROLLED_PROCESS_BACKEND_HPP
#define WUWE_AGENT_EXECUTION_CONTROLLED_PROCESS_BACKEND_HPP

#include <filesystem>
#include <memory>

#include <wuwe/agent/execution/execution_backend.hpp>

namespace wuwe::agent::execution {

struct controlled_process_backend_config {
  std::filesystem::path python_interpreter { "python" };
  std::filesystem::path fallback_workdir;
  bool use_job_object { true };
};

class controlled_process_backend final : public execution_backend {
public:
  explicit controlled_process_backend(
    controlled_process_backend_config config = {});

  [[nodiscard]] sandbox::sandbox_backend_info info() const override;

  [[nodiscard]] execution_result run(
    const execution_request& request,
    std::stop_token stop_token) override;

private:
  controlled_process_backend_config config_;
};

[[nodiscard]] std::unique_ptr<execution_backend> make_controlled_process_backend(
  controlled_process_backend_config config = {});

} // namespace wuwe::agent::execution

#endif // WUWE_AGENT_EXECUTION_CONTROLLED_PROCESS_BACKEND_HPP
