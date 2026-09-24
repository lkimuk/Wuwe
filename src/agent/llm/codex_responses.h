#pragma once

#include <map>
#include <set>
#include <wuwe/agent/llm/codex_llm_client.h>
#include <wuwe/net/sse_event_parser.h>

WUWE_NAMESPACE_BEGIN
namespace codex_responses {
using json = nlohmann::json;
struct payload_result {
  json body;
  std::error_code error;
  std::string unsupported;
};
payload_result build_payload(
  const llm_request& request, const codex_llm_options& options, const oauth_account_info& account);

// One decoder per request; no conversation state is retained in the client.
class decoder {
public:
  decoder(const codex_llm_options& options, const llm_request& request,
    const oauth_account_info& account, std::string model,
    std::function<void(llm_stream_event)> emit);
  bool event(const sse_event& value);
  bool completed() const noexcept {
    return completed_;
  }
  bool terminal() const noexcept {
    return completed_ || static_cast<bool>(result.error_code);
  }
  llm_response result;

private:
  struct item_state {
    std::string id;
    std::string type;
    llm_tool_call call;
    std::map<int, std::string> text;
    std::map<int, std::string> summary;
    bool arguments_seen { false };
    bool done { false };
    json final_item;
  };
  const codex_llm_options& options_;
  const llm_request& request_;
  const oauth_account_info& account_;
  std::string model_;
  std::function<void(llm_stream_event)> emit_;
  std::map<int, item_state> items_;
  std::map<std::string, int> ids_;
  std::string response_id_;
  bool completed_ { false };
  bool fail(agent::llm_error_code error = agent::llm_error_code::invalid_response);
  item_state* item(int index, const std::string& id, const std::string& type);
  bool finalize_item(int index, const json& value);
  bool complete(const json& response);
};
} // namespace codex_responses
WUWE_NAMESPACE_END
