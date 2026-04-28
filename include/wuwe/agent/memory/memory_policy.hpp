#ifndef WUWE_AGENT_MEMORY_POLICY_HPP
#define WUWE_AGENT_MEMORY_POLICY_HPP

#include <cstddef>
#include <string>

namespace wuwe::agent::memory {

struct memory_policy {
  std::size_t max_recent_messages { 12 };
  std::size_t max_working_records { 16 };
  std::size_t max_long_term_records { 8 };
  std::size_t max_summary_records { 2 };

  std::size_t max_memory_chars { 6000 };
  std::size_t max_record_chars { 1200 };

  bool include_conversation { true };
  bool include_working { true };
  bool include_summaries { true };
  bool include_long_term { true };

  bool inject_as_system_message { true };
  std::string injection_header { "Relevant memory:" };
};

} // namespace wuwe::agent::memory

#endif // WUWE_AGENT_MEMORY_POLICY_HPP
