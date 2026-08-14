#ifndef WUWE_AGENT_TRAINING_TRAINING_ERROR_HPP
#define WUWE_AGENT_TRAINING_TRAINING_ERROR_HPP

#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

#include <nlohmann/json.hpp>

namespace wuwe::agent::training {

enum class training_remote_outcome {
  not_applicable,
  not_applied,
  applied,
  uncertain,
};

enum class training_remote_effect {
  read_only,
  mutation,
};

[[nodiscard]] inline std::string to_string(training_remote_outcome outcome) {
  switch (outcome) {
    case training_remote_outcome::not_applicable:
      return "not_applicable";
    case training_remote_outcome::not_applied:
      return "not_applied";
    case training_remote_outcome::applied:
      return "applied";
    case training_remote_outcome::uncertain:
      return "uncertain";
  }
  return "uncertain";
}

enum class training_errc {
  invalid_request,
  not_found,
  invalid_transition,
  revision_conflict,
  provider_transport,
  provider_protocol,
  authorization_denied,
  persistence_failure,
  corrupted_state,
  cancelled,
  timed_out,
  unsupported_operation,
  reconciliation_required,
};

[[nodiscard]] inline std::string to_string(training_errc code) {
  switch (code) {
    case training_errc::invalid_request:
      return "invalid_request";
    case training_errc::not_found:
      return "not_found";
    case training_errc::invalid_transition:
      return "invalid_transition";
    case training_errc::revision_conflict:
      return "revision_conflict";
    case training_errc::provider_transport:
      return "provider_transport";
    case training_errc::provider_protocol:
      return "provider_protocol";
    case training_errc::authorization_denied:
      return "authorization_denied";
    case training_errc::persistence_failure:
      return "persistence_failure";
    case training_errc::corrupted_state:
      return "corrupted_state";
    case training_errc::cancelled:
      return "cancelled";
    case training_errc::timed_out:
      return "timed_out";
    case training_errc::unsupported_operation:
      return "unsupported_operation";
    case training_errc::reconciliation_required:
      return "reconciliation_required";
  }
  return "corrupted_state";
}

struct training_error {
  training_errc code { training_errc::corrupted_state };
  std::string message;
  bool retryable { false };
  nlohmann::json details = nlohmann::json::object();
  std::map<std::string, std::string> metadata;
  training_remote_outcome remote_outcome { training_remote_outcome::not_applicable };
};

[[nodiscard]] inline bool ambiguous_training_error(training_errc code) noexcept {
  return code == training_errc::provider_transport || code == training_errc::provider_protocol ||
         code == training_errc::cancelled || code == training_errc::timed_out ||
         code == training_errc::reconciliation_required;
}

[[nodiscard]] inline bool valid_training_remote_outcome(
  training_errc code, training_remote_outcome outcome) noexcept {
  if (code == training_errc::reconciliation_required) {
    return outcome == training_remote_outcome::applied ||
           outcome == training_remote_outcome::uncertain;
  }
  if (outcome == training_remote_outcome::applied ||
      outcome == training_remote_outcome::uncertain) {
    return ambiguous_training_error(code);
  }
  return true;
}

inline void validate_training_error(const training_error& error) {
  if (error.message.empty())
    throw std::invalid_argument("training error message must not be empty");
  if (!error.details.is_object())
    throw std::invalid_argument("training error details must be an object");
  if (!valid_training_remote_outcome(error.code, error.remote_outcome))
    throw std::invalid_argument("training error code and remote outcome are inconsistent");
}

inline void validate_training_error(const training_error& error, training_remote_effect effect) {
  validate_training_error(error);
  if (effect == training_remote_effect::read_only &&
      error.remote_outcome != training_remote_outcome::not_applicable) {
    throw std::invalid_argument("read-only training errors cannot classify a remote mutation");
  }
  if (effect == training_remote_effect::mutation &&
      error.remote_outcome == training_remote_outcome::not_applicable) {
    throw std::invalid_argument("training mutation errors must classify the remote outcome");
  }
}

template<typename T>
class training_result {
public:
  [[nodiscard]] static training_result success(T value) {
    return training_result(std::move(value));
  }

  [[nodiscard]] static training_result failure(training_error error) {
    validate_training_error(error);
    return training_result(std::move(error));
  }

  [[nodiscard]] explicit operator bool() const noexcept {
    return std::holds_alternative<T>(storage_);
  }

  [[nodiscard]] const T* value_if() const noexcept {
    return std::get_if<T>(&storage_);
  }
  [[nodiscard]] T* value_if() noexcept {
    return std::get_if<T>(&storage_);
  }
  [[nodiscard]] const training_error* error_if() const noexcept {
    return std::get_if<training_error>(&storage_);
  }
  [[nodiscard]] training_error* error_if() noexcept {
    return std::get_if<training_error>(&storage_);
  }

private:
  explicit training_result(T value) : storage_(std::move(value)) {
  }
  explicit training_result(training_error error) : storage_(std::move(error)) {
  }
  std::variant<T, training_error> storage_;
};

} // namespace wuwe::agent::training

#endif // WUWE_AGENT_TRAINING_TRAINING_ERROR_HPP
