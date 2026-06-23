#include "restricted_process_backend_candidate.hpp"

#include <utility>

#ifdef _WIN32
#include "restricted_process_execution_plan_win32.hpp"
#endif

namespace wuwe::agent::execution::detail {
namespace {

class restricted_process_backend_candidate final : public execution_backend {
public:
  explicit restricted_process_backend_candidate(
    restricted_process_backend_config config)
      : config_(std::move(config)) {
  }

  [[nodiscard]] sandbox::sandbox_backend_info info() const override {
    auto descriptor = restricted_process_backend_descriptor();
    const auto availability =
      evaluate_restricted_process_backend_availability(config_);
    descriptor.unavailable_reason =
      "restricted_process backend candidate is internal and not registered";
    descriptor.enforcement = availability.contract;
    return descriptor;
  }

  [[nodiscard]] execution_result run(
    const execution_request& request,
    std::stop_token stop_token) override {
#ifdef _WIN32
    auto result = run_restricted_execution_plan(config_, request, stop_token);
    result.metadata["backend_candidate"] = "true";
    return result;
#else
    execution_result result {
      .termination_reason = execution_termination_reason::backend_error,
      .error_message = "restricted_process backend candidate is Windows-only",
    };
    result.metadata["backend_name"] = "restricted_process";
    result.metadata["backend_candidate"] = "true";
    result.metadata["error_code"] = "restricted_process_unsupported_platform";
    return result;
#endif
  }

private:
  restricted_process_backend_config config_;
};

} // namespace

std::unique_ptr<execution_backend> make_restricted_process_backend_candidate(
  restricted_process_backend_config config) {
  return std::make_unique<restricted_process_backend_candidate>(
    std::move(config));
}

} // namespace wuwe::agent::execution::detail
