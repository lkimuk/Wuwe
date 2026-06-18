#ifndef WUWE_AGENT_AUDIT_AUDIT_HPP
#define WUWE_AGENT_AUDIT_AUDIT_HPP

#include <chrono>
#include <map>
#include <string>

namespace wuwe::agent::audit {

enum class audit_event_outcome {
  attempted,
  allowed,
  denied,
  approved,
  started,
  completed,
  failed,
  cancelled,
  timed_out,
};

struct audit_event {
  std::string module;
  std::string name;
  std::string id;
  std::string trace_id;
  std::string subject_id;
  audit_event_outcome outcome { audit_event_outcome::attempted };
  std::chrono::system_clock::time_point timestamp {
    std::chrono::system_clock::now()
  };
  std::chrono::milliseconds elapsed { 0 };
  std::map<std::string, std::string> attributes;
};

[[nodiscard]] inline std::string to_string(audit_event_outcome outcome) {
  switch (outcome) {
    case audit_event_outcome::attempted:
      return "attempted";
    case audit_event_outcome::allowed:
      return "allowed";
    case audit_event_outcome::denied:
      return "denied";
    case audit_event_outcome::approved:
      return "approved";
    case audit_event_outcome::started:
      return "started";
    case audit_event_outcome::completed:
      return "completed";
    case audit_event_outcome::failed:
      return "failed";
    case audit_event_outcome::cancelled:
      return "cancelled";
    case audit_event_outcome::timed_out:
      return "timed_out";
  }
  return "unknown";
}

} // namespace wuwe::agent::audit

#endif // WUWE_AGENT_AUDIT_AUDIT_HPP
