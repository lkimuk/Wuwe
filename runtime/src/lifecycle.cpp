#include <wuwe/runtime_host/lifecycle.hpp>

#include <utility>

namespace wuwe::runtime_host {

std::string to_string(lifecycle_state state) {
  switch (state) {
    case lifecycle_state::created:
      return "created";
    case lifecycle_state::configured:
      return "configured";
    case lifecycle_state::starting:
      return "starting";
    case lifecycle_state::ready:
      return "ready";
    case lifecycle_state::draining:
      return "draining";
    case lifecycle_state::stopped:
      return "stopped";
    case lifecycle_state::failed:
      return "failed";
  }
  return "failed";
}

bool terminal(lifecycle_state state) noexcept {
  return state == lifecycle_state::stopped || state == lifecycle_state::failed;
}

bool valid_transition(lifecycle_state from, lifecycle_state to) noexcept {
  if (from == to)
    return false;
  if (to == lifecycle_state::failed)
    return !terminal(from);
  switch (from) {
    case lifecycle_state::created:
      return to == lifecycle_state::configured || to == lifecycle_state::stopped;
    case lifecycle_state::configured:
      return to == lifecycle_state::starting || to == lifecycle_state::stopped;
    case lifecycle_state::starting:
      return to == lifecycle_state::ready || to == lifecycle_state::draining;
    case lifecycle_state::ready:
      return to == lifecycle_state::draining;
    case lifecycle_state::draining:
      return to == lifecycle_state::stopped;
    case lifecycle_state::stopped:
    case lifecycle_state::failed:
      return false;
  }
  return false;
}

lifecycle_snapshot lifecycle::snapshot() const {
  std::scoped_lock lock(mutex_);
  return snapshot_;
}

lifecycle_state lifecycle::state() const {
  std::scoped_lock lock(mutex_);
  return snapshot_.state;
}

void lifecycle::transition(lifecycle_state next) {
  std::scoped_lock lock(mutex_);
  if (!valid_transition(snapshot_.state, next)) {
    throw lifecycle_error("invalid runtime lifecycle transition from " +
                          to_string(snapshot_.state) + " to " + to_string(next));
  }
  snapshot_.state = next;
  snapshot_.changed_at = std::chrono::system_clock::now();
  snapshot_.failure.clear();
}

void lifecycle::fail(std::string message) {
  std::scoped_lock lock(mutex_);
  if (terminal(snapshot_.state)) {
    if (snapshot_.state == lifecycle_state::failed && snapshot_.failure.empty())
      snapshot_.failure = std::move(message);
    return;
  }
  snapshot_.state = lifecycle_state::failed;
  snapshot_.changed_at = std::chrono::system_clock::now();
  snapshot_.failure = std::move(message);
}

} // namespace wuwe::runtime_host
