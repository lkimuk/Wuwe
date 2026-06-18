#ifndef WUWE_AGENT_CAPABILITY_CAPABILITY_HPP
#define WUWE_AGENT_CAPABILITY_CAPABILITY_HPP

#include <map>
#include <string>
#include <vector>

namespace wuwe::agent::capability {

enum class capability_risk_level {
  low,
  medium,
  high,
  critical,
};

struct capability_request {
  std::string name;
  capability_risk_level risk { capability_risk_level::low };
  std::string summary;
  std::vector<std::string> resources;
  std::string tool_name;
  std::string trace_id;
  std::string subject_id;
  std::map<std::string, std::string> metadata;
};

[[nodiscard]] inline std::string to_string(capability_risk_level risk) {
  switch (risk) {
    case capability_risk_level::low:
      return "low";
    case capability_risk_level::medium:
      return "medium";
    case capability_risk_level::high:
      return "high";
    case capability_risk_level::critical:
      return "critical";
  }
  return "unknown";
}

namespace names {

inline constexpr const char* process_python = "process.python";
inline constexpr const char* process_shell = "process.shell";
inline constexpr const char* filesystem_read = "filesystem.read";
inline constexpr const char* filesystem_write = "filesystem.write";
inline constexpr const char* network_outbound = "network.outbound";
inline constexpr const char* environment_read = "environment.read";
inline constexpr const char* secret_read = "secret.read";

} // namespace names

} // namespace wuwe::agent::capability

#endif // WUWE_AGENT_CAPABILITY_CAPABILITY_HPP
