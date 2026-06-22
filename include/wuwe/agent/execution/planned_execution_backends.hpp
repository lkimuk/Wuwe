#ifndef WUWE_AGENT_EXECUTION_PLANNED_EXECUTION_BACKENDS_HPP
#define WUWE_AGENT_EXECUTION_PLANNED_EXECUTION_BACKENDS_HPP

#include <memory>
#include <string>

#include <wuwe/agent/execution/execution_backend.hpp>

namespace wuwe::agent::execution {

struct planned_backend_config {
  std::string unavailable_reason;
};

class restricted_process_backend final : public execution_backend {
public:
  explicit restricted_process_backend(planned_backend_config config = {});

  [[nodiscard]] sandbox::sandbox_backend_info info() const override;

  [[nodiscard]] execution_result run(
    const execution_request& request,
    std::stop_token stop_token) override;

private:
  planned_backend_config config_;
};

class container_backend final : public execution_backend {
public:
  explicit container_backend(planned_backend_config config = {});

  [[nodiscard]] sandbox::sandbox_backend_info info() const override;

  [[nodiscard]] execution_result run(
    const execution_request& request,
    std::stop_token stop_token) override;

private:
  planned_backend_config config_;
};

class wasm_backend final : public execution_backend {
public:
  explicit wasm_backend(planned_backend_config config = {});

  [[nodiscard]] sandbox::sandbox_backend_info info() const override;

  [[nodiscard]] execution_result run(
    const execution_request& request,
    std::stop_token stop_token) override;

private:
  planned_backend_config config_;
};

[[nodiscard]] std::unique_ptr<execution_backend> make_restricted_process_backend(
  planned_backend_config config = {});

[[nodiscard]] std::unique_ptr<execution_backend> make_container_backend(
  planned_backend_config config = {});

[[nodiscard]] std::unique_ptr<execution_backend> make_wasm_backend(
  planned_backend_config config = {});

} // namespace wuwe::agent::execution

#endif // WUWE_AGENT_EXECUTION_PLANNED_EXECUTION_BACKENDS_HPP
