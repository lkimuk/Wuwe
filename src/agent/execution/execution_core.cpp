#include <wuwe/agent/execution/execution_core.hpp>

namespace wuwe::agent::execution {

std::string to_string(execution_language language) {
  switch (language) {
    case execution_language::python:
      return "python";
  }
  return "unknown";
}

std::string to_string(execution_termination_reason reason) {
  switch (reason) {
    case execution_termination_reason::exited:
      return "exited";
    case execution_termination_reason::timeout:
      return "timeout";
    case execution_termination_reason::cancelled:
      return "cancelled";
    case execution_termination_reason::launch_failed:
      return "launch_failed";
    case execution_termination_reason::policy_denied:
      return "policy_denied";
    case execution_termination_reason::approval_denied:
      return "approval_denied";
    case execution_termination_reason::backend_error:
      return "backend_error";
  }
  return "unknown";
}

} // namespace wuwe::agent::execution
