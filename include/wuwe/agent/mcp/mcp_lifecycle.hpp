#ifndef WUWE_AGENT_MCP_LIFECYCLE_HPP
#define WUWE_AGENT_MCP_LIFECYCLE_HPP

#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <wuwe/agent/mcp/mcp_protocol.hpp>

namespace wuwe::agent::mcp {

enum class mcp_request_state {
  pending,
  running,
  completed,
  failed,
  cancelled,
};

struct mcp_request_record {
  std::string id;
  std::string method;
  std::string target;
  mcp_request_state state { mcp_request_state::pending };
  std::string error;
  json params { json::object() };
  json progress_token;
  double progress { 0.0 };
  std::optional<double> total;
  std::string progress_message;
  std::chrono::system_clock::time_point started_at {};
  std::chrono::system_clock::time_point finished_at {};
  std::chrono::milliseconds timeout { 0 };
};

class mcp_request_registry {
public:
  void start(
    std::string id,
    std::string method,
    std::string target,
    json params,
    std::chrono::milliseconds timeout = std::chrono::milliseconds { 0 }) {
    std::lock_guard lock(mutex_);
    mcp_request_record record;
    record.id = id;
    record.method = std::move(method);
    record.target = std::move(target);
    record.state = mcp_request_state::running;
    record.params = std::move(params);
    record.started_at = std::chrono::system_clock::now();
    record.timeout = timeout;
    records_[std::move(id)] = std::move(record);
  }

  void complete(const std::string& id) {
    std::lock_guard lock(mutex_);
    finish(id, mcp_request_state::completed, {});
  }

  void fail(const std::string& id, std::string error) {
    std::lock_guard lock(mutex_);
    finish(id, mcp_request_state::failed, std::move(error));
  }

  void cancel(const std::string& id, std::string reason = {}) {
    std::lock_guard lock(mutex_);
    finish(id, mcp_request_state::cancelled, std::move(reason));
  }

  void progress(
    const std::string& id,
    json progress_token,
    double value,
    std::optional<double> total = std::nullopt,
    std::string message = {}) {
    std::lock_guard lock(mutex_);
    const auto it = records_.find(id);
    if (it == records_.end()) {
      return;
    }
    it->second.progress_token = std::move(progress_token);
    it->second.progress = value;
    it->second.total = total;
    it->second.progress_message = std::move(message);
  }

  bool timed_out(const std::string& id) const {
    std::lock_guard lock(mutex_);
    const auto it = records_.find(id);
    if (it == records_.end() || it->second.timeout.count() <= 0 ||
        it->second.state != mcp_request_state::running) {
      return false;
    }
    return std::chrono::system_clock::now() - it->second.started_at > it->second.timeout;
  }

  std::optional<mcp_request_record> get(const std::string& id) const {
    std::lock_guard lock(mutex_);
    const auto it = records_.find(id);
    if (it == records_.end()) {
      return std::nullopt;
    }
    return it->second;
  }

  std::vector<mcp_request_record> records() const {
    std::lock_guard lock(mutex_);
    std::vector<mcp_request_record> output;
    output.reserve(records_.size());
    for (const auto& [_, record] : records_) {
      output.push_back(record);
    }
    return output;
  }

  void clear_completed() {
    std::lock_guard lock(mutex_);
    for (auto it = records_.begin(); it != records_.end();) {
      if (it->second.state == mcp_request_state::completed ||
          it->second.state == mcp_request_state::failed ||
          it->second.state == mcp_request_state::cancelled) {
        it = records_.erase(it);
      }
      else {
        ++it;
      }
    }
  }

private:
  void finish(const std::string& id, mcp_request_state state, std::string error) {
    const auto it = records_.find(id);
    if (it == records_.end()) {
      return;
    }
    it->second.state = state;
    it->second.error = std::move(error);
    it->second.finished_at = std::chrono::system_clock::now();
  }

  std::map<std::string, mcp_request_record> records_;
  mutable std::mutex mutex_;
};

} // namespace wuwe::agent::mcp

#endif // WUWE_AGENT_MCP_LIFECYCLE_HPP
