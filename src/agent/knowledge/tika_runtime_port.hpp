#ifndef WUWE_AGENT_KNOWLEDGE_TIKA_RUNTIME_PORT_HPP
#define WUWE_AGENT_KNOWLEDGE_TIKA_RUNTIME_PORT_HPP

#include <string>

namespace wuwe::agent::knowledge::detail {

int select_available_loopback_port(const std::string& host);

} // namespace wuwe::agent::knowledge::detail

#endif // WUWE_AGENT_KNOWLEDGE_TIKA_RUNTIME_PORT_HPP
