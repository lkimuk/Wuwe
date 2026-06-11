#ifndef WUWE_AGENT_REASONING_CORE_HPP
#define WUWE_AGENT_REASONING_CORE_HPP

#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <wuwe/agent/llm/llm_types.h>
#include <wuwe/agent/planning/plan.hpp>
#include <wuwe/agent/reflection/reflection_core.hpp>
#include <wuwe/agent/reflection/reflection_runner.hpp>
#include <wuwe/agent/tools/tool.hpp>

namespace wuwe::agent::reasoning {

enum class reasoning_mode {
  simple,
  react,
  reflect_and_retry,
  plan_execute,
};

inline std::string to_string(reasoning_mode mode) {
  switch (mode) {
    case reasoning_mode::simple:
      return "simple";
    case reasoning_mode::react:
      return "react";
    case reasoning_mode::reflect_and_retry:
      return "reflect_and_retry";
    case reasoning_mode::plan_execute:
      return "plan_execute";
  }
  return "unknown";
}

enum class reasoning_event_type {
  started,
  model_started,
  content_delta,
  tool_started,
  tool_completed,
  reflection_started,
  reflection_completed,
  plan_created,
  plan_step_started,
  plan_step_completed,
  plan_step_failed,
  plan_step_blocked,
  plan_revised,
  completed,
  failed,
  cancelled,
};

inline std::string to_string(reasoning_event_type type) {
  switch (type) {
    case reasoning_event_type::started:
      return "started";
    case reasoning_event_type::model_started:
      return "model_started";
    case reasoning_event_type::content_delta:
      return "content_delta";
    case reasoning_event_type::tool_started:
      return "tool_started";
    case reasoning_event_type::tool_completed:
      return "tool_completed";
    case reasoning_event_type::reflection_started:
      return "reflection_started";
    case reasoning_event_type::reflection_completed:
      return "reflection_completed";
    case reasoning_event_type::plan_created:
      return "plan_created";
    case reasoning_event_type::plan_step_started:
      return "plan_step_started";
    case reasoning_event_type::plan_step_completed:
      return "plan_step_completed";
    case reasoning_event_type::plan_step_failed:
      return "plan_step_failed";
    case reasoning_event_type::plan_step_blocked:
      return "plan_step_blocked";
    case reasoning_event_type::plan_revised:
      return "plan_revised";
    case reasoning_event_type::completed:
      return "completed";
    case reasoning_event_type::failed:
      return "failed";
    case reasoning_event_type::cancelled:
      return "cancelled";
  }
  return "unknown";
}

struct reasoning_budget {
  std::size_t max_steps { 8 };
  std::size_t max_model_calls { 8 };
  std::size_t max_tool_calls { 16 };
  std::size_t max_tool_rounds { 4 };
  std::size_t max_reflection_attempts { 2 };
  std::chrono::milliseconds timeout { 0 };
};

struct reasoning_policy {
  reasoning_mode mode { reasoning_mode::react };
  reasoning_budget budget;
  bool enable_streaming { true };
  bool enable_reflection { false };
  bool allow_replanning { false };
  bool return_revised_output { true };
  planning::plan_policy planning;
  reflection::reflection_policy reflection;
};

struct reasoning_request {
  std::string input;
  std::string system_prompt;
  std::string model;
  double temperature { 0.2 };
  reasoning_policy policy;
  reflection::reflection_rubric rubric;
  std::map<std::string, std::string> metadata;
};

struct reasoning_step {
  std::string id;
  std::string type;
  std::string input;
  std::string output;
  std::string error;
  std::map<std::string, std::string> metadata;
};

struct reasoning_trace_record {
  std::size_t sequence { 0 };
  reasoning_event_type type { reasoning_event_type::started };
  reasoning_mode mode { reasoning_mode::simple };
  std::string step_id;
  std::string message;
  std::string delta;
  std::string error;
  std::chrono::milliseconds elapsed { 0 };
  std::map<std::string, std::string> metadata;
};

struct reasoning_usage {
  std::size_t model_calls { 0 };
  std::size_t tool_calls { 0 };
  std::size_t reflection_calls { 0 };
  std::size_t plan_steps { 0 };
};

struct reasoning_result {
  reasoning_mode mode { reasoning_mode::simple };
  bool completed { false };
  std::string content;
  llm_response final_response;
  std::optional<planning::plan_run_result> plan;
  std::vector<reflection::reflection_run_result> reflections;
  std::vector<reasoning_step> steps;
  std::vector<reasoning_trace_record> trace;
  reasoning_usage usage;
  std::error_code error_code;
  std::string error;
  std::chrono::milliseconds elapsed { 0 };

  explicit operator bool() const noexcept {
    return completed && !error_code;
  }
};

struct reasoning_event {
  reasoning_event_type type { reasoning_event_type::started };
  reasoning_mode mode { reasoning_mode::simple };
  std::string message;
  std::string delta;
  std::string step_id;
  const llm_tool_call* tool_call {};
  const llm_tool_result* tool_result {};
  const planning::plan* current_plan {};
  const planning::plan_step* plan_step {};
  const reflection::reflection_result* reflection_result {};
  const reasoning_result* result {};
  std::chrono::milliseconds elapsed { 0 };
  std::map<std::string, std::string> metadata;
};

using reasoning_observer = std::function<void(const reasoning_event&)>;

} // namespace wuwe::agent::reasoning

#endif // WUWE_AGENT_REASONING_CORE_HPP
