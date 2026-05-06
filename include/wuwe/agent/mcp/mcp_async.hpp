#ifndef WUWE_AGENT_MCP_ASYNC_HPP
#define WUWE_AGENT_MCP_ASYNC_HPP

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <wuwe/agent/mcp/mcp_lifecycle.hpp>

namespace wuwe::agent::mcp {

struct mcp_async_cancel_token {
  std::shared_ptr<std::atomic_bool> cancelled;

  bool is_cancelled() const noexcept {
    return cancelled && cancelled->load();
  }
};

struct mcp_async_task_snapshot {
  mcp_request_record record;
  bool ready { false };
};

class mcp_async_task_registry {
public:
  template<typename Function>
  std::string submit(
    std::string id,
    std::string method,
    std::string target,
    json params,
    Function&& function,
    std::chrono::milliseconds timeout = std::chrono::milliseconds { 0 }) {
    auto state = std::make_shared<task_state>();
    state->cancelled = std::make_shared<std::atomic_bool>(false);
    state->record.id = id;
    state->record.method = std::move(method);
    state->record.target = std::move(target);
    state->record.params = std::move(params);
    state->record.state = mcp_request_state::running;
    state->record.started_at = std::chrono::system_clock::now();
    state->record.timeout = timeout;

    const auto token = mcp_async_cancel_token { state->cancelled };
    state->future = std::async(std::launch::async,
      [state, token, function = std::forward<Function>(function)]() mutable {
        try {
          function(token);
          std::lock_guard lock(state->mutex);
          if (state->record.state == mcp_request_state::running) {
            state->record.state = token.is_cancelled()
                                    ? mcp_request_state::cancelled
                                    : mcp_request_state::completed;
            state->record.finished_at = std::chrono::system_clock::now();
          }
        }
        catch (const std::exception& ex) {
          std::lock_guard lock(state->mutex);
          state->record.state = mcp_request_state::failed;
          state->record.error = ex.what();
          state->record.finished_at = std::chrono::system_clock::now();
        }
        catch (...) {
          std::lock_guard lock(state->mutex);
          state->record.state = mcp_request_state::failed;
          state->record.error = "unknown async task failure";
          state->record.finished_at = std::chrono::system_clock::now();
        }
      });

    std::lock_guard lock(mutex_);
    tasks_[id] = std::move(state);
    return id;
  }

  bool cancel(const std::string& id, std::string reason = {}) {
    const auto task = find_task(id);
    if (!task) {
      return false;
    }
    task->cancelled->store(true);
    std::lock_guard lock(task->mutex);
    if (task->record.state == mcp_request_state::running) {
      task->record.state = mcp_request_state::cancelled;
      task->record.error = std::move(reason);
      task->record.finished_at = std::chrono::system_clock::now();
    }
    return true;
  }

  void progress(
    const std::string& id,
    json progress_token,
    double value,
    std::optional<double> total = std::nullopt,
    std::string message = {}) {
    const auto task = find_task(id);
    if (!task) {
      return;
    }
    std::lock_guard lock(task->mutex);
    task->record.progress_token = std::move(progress_token);
    task->record.progress = value;
    task->record.total = total;
    task->record.progress_message = std::move(message);
  }

  std::optional<mcp_async_task_snapshot> poll(const std::string& id) {
    const auto task = find_task(id);
    if (!task) {
      return std::nullopt;
    }
    enforce_timeout(*task);
    std::lock_guard lock(task->mutex);
    return mcp_async_task_snapshot {
      .record = task->record,
      .ready = task->future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready,
    };
  }

  std::vector<mcp_async_task_snapshot> snapshots() {
    std::vector<std::shared_ptr<task_state>> tasks;
    {
      std::lock_guard lock(mutex_);
      tasks.reserve(tasks_.size());
      for (const auto& [_, task] : tasks_) {
        tasks.push_back(task);
      }
    }

    std::vector<mcp_async_task_snapshot> output;
    output.reserve(tasks.size());
    for (const auto& task : tasks) {
      enforce_timeout(*task);
      std::lock_guard lock(task->mutex);
      output.push_back({
        .record = task->record,
        .ready = task->future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready,
      });
    }
    return output;
  }

  void clear_finished() {
    std::lock_guard lock(mutex_);
    for (auto it = tasks_.begin(); it != tasks_.end();) {
      const auto& task = it->second;
      std::lock_guard task_lock(task->mutex);
      const auto ready = task->future.wait_for(std::chrono::milliseconds(0)) ==
                         std::future_status::ready;
      if (ready &&
          (task->record.state == mcp_request_state::completed ||
           task->record.state == mcp_request_state::failed ||
           task->record.state == mcp_request_state::cancelled)) {
        it = tasks_.erase(it);
      }
      else {
        ++it;
      }
    }
  }

private:
  struct task_state {
    mutable std::mutex mutex;
    mcp_request_record record;
    std::shared_ptr<std::atomic_bool> cancelled;
    std::future<void> future;
  };

  std::shared_ptr<task_state> find_task(const std::string& id) const {
    std::lock_guard lock(mutex_);
    const auto it = tasks_.find(id);
    if (it == tasks_.end()) {
      return {};
    }
    return it->second;
  }

  static void enforce_timeout(task_state& task) {
    std::lock_guard lock(task.mutex);
    if (task.record.timeout.count() <= 0 ||
        task.record.state != mcp_request_state::running) {
      return;
    }
    if (std::chrono::system_clock::now() - task.record.started_at <= task.record.timeout) {
      return;
    }
    task.cancelled->store(true);
    task.record.state = mcp_request_state::failed;
    task.record.error = "request timed out";
    task.record.finished_at = std::chrono::system_clock::now();
  }

  mutable std::mutex mutex_;
  std::map<std::string, std::shared_ptr<task_state>> tasks_;
};

} // namespace wuwe::agent::mcp

#endif // WUWE_AGENT_MCP_ASYNC_HPP
