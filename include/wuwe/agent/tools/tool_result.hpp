#ifndef WUWE_AGENT_TOOL_RESULT_HPP
#define WUWE_AGENT_TOOL_RESULT_HPP

#include <string>

#include <wuwe/common/wuwe_fwd.h>

WUWE_NAMESPACE_BEGIN

struct tool_result {
  std::string call_id;
  std::string name;
  std::string result_json;
  bool is_error = false;
};

WUWE_NAMESPACE_END

#endif // WUWE_AGENT_TOOL_RESULT_HPP
