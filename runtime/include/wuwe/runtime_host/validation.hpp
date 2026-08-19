#ifndef WUWE_RUNTIME_HOST_VALIDATION_HPP
#define WUWE_RUNTIME_HOST_VALIDATION_HPP

#include <stdexcept>
#include <string>
#include <vector>

namespace wuwe::runtime_host {

struct validation_issue {
  std::string path;
  std::string code;
  std::string message;
};

struct validation_report {
  std::vector<validation_issue> issues;

  [[nodiscard]] bool valid() const noexcept {
    return issues.empty();
  }

  [[nodiscard]] explicit operator bool() const noexcept {
    return valid();
  }
};

class validation_error final : public std::invalid_argument {
public:
  validation_error(std::string subject, validation_report report);

  [[nodiscard]] const std::string& subject() const noexcept {
    return subject_;
  }

  [[nodiscard]] const validation_report& report() const noexcept {
    return report_;
  }

private:
  std::string subject_;
  validation_report report_;
};

} // namespace wuwe::runtime_host

#endif // WUWE_RUNTIME_HOST_VALIDATION_HPP
