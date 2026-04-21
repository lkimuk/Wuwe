#ifndef WUWE_AGENT_TOOL_CALL_HPP
#define WUWE_AGENT_TOOL_CALL_HPP

#include <string>

#include <wuwe/common/wuwe_fwd.h>

WUWE_NAMESPACE_BEGIN

struct tool_call {
  std::string id;
  std::string name;
  std::string arguments_json;
};

WUWE_NAMESPACE_END

#endif // WUWE_AGENT_TOOL_CALL_HPP
