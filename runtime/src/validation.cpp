#include <wuwe/runtime_host/validation.hpp>

#include <utility>

namespace wuwe::runtime_host {
namespace {

std::string validation_message(const std::string& subject, const validation_report& report) {
  const auto prefix = subject + " is invalid";
  if (report.issues.empty())
    return prefix;
  const auto& first = report.issues.front();
  return prefix + " at " + first.path + ": " + first.message;
}

} // namespace

validation_error::validation_error(std::string subject, validation_report report)
    : std::invalid_argument(validation_message(subject, report)), subject_(std::move(subject)),
      report_(std::move(report)) {
}

} // namespace wuwe::runtime_host
