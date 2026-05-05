#ifndef WUWE_AGENT_KNOWLEDGE_HASH_HPP
#define WUWE_AGENT_KNOWLEDGE_HASH_HPP

#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>

namespace wuwe::agent::knowledge {

inline std::string stable_hash(std::string_view text) {
  std::uint64_t hash = 14695981039346656037ull;
  for (const unsigned char ch : text) {
    hash ^= ch;
    hash *= 1099511628211ull;
  }

  std::ostringstream output;
  output << std::hex << std::setw(16) << std::setfill('0') << hash;
  return output.str();
}

} // namespace wuwe::agent::knowledge

#endif // WUWE_AGENT_KNOWLEDGE_HASH_HPP
