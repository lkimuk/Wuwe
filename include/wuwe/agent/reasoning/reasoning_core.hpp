#ifndef WUWE_AGENT_REASONING_CORE_HPP
#define WUWE_AGENT_REASONING_CORE_HPP

#include <chrono>
#include <cctype>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <stop_token>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

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

enum class reasoning_task_profile {
  simple_answer,
  tool_required,
  complex_analysis,
  high_confidence_answer,
  plan_required,
  auto_select,
};

inline std::string to_string(reasoning_task_profile profile) {
  switch (profile) {
    case reasoning_task_profile::simple_answer:
      return "simple_answer";
    case reasoning_task_profile::tool_required:
      return "tool_required";
    case reasoning_task_profile::complex_analysis:
      return "complex_analysis";
    case reasoning_task_profile::high_confidence_answer:
      return "high_confidence_answer";
    case reasoning_task_profile::plan_required:
      return "plan_required";
    case reasoning_task_profile::auto_select:
      return "auto_select";
  }
  return "unknown";
}

enum class reasoning_error_code {
  none,
  cancelled,
  timeout,
  model_call_budget_exceeded,
  tool_call_budget_exceeded,
  reflection_budget_exceeded,
  planning_budget_exceeded,
  missing_api_key,
  authentication_failed,
  rate_limited,
  model_unavailable,
  tool_failed,
  reflection_blocked,
  planning_failed,
  invalid_configuration,
  transport_failed,
  unknown,
};

inline std::string to_string(reasoning_error_code code) {
  switch (code) {
    case reasoning_error_code::none:
      return "none";
    case reasoning_error_code::cancelled:
      return "cancelled";
    case reasoning_error_code::timeout:
      return "timeout";
    case reasoning_error_code::model_call_budget_exceeded:
      return "model_call_budget_exceeded";
    case reasoning_error_code::tool_call_budget_exceeded:
      return "tool_call_budget_exceeded";
    case reasoning_error_code::reflection_budget_exceeded:
      return "reflection_budget_exceeded";
    case reasoning_error_code::planning_budget_exceeded:
      return "planning_budget_exceeded";
    case reasoning_error_code::missing_api_key:
      return "missing_api_key";
    case reasoning_error_code::authentication_failed:
      return "authentication_failed";
    case reasoning_error_code::rate_limited:
      return "rate_limited";
    case reasoning_error_code::model_unavailable:
      return "model_unavailable";
    case reasoning_error_code::tool_failed:
      return "tool_failed";
    case reasoning_error_code::reflection_blocked:
      return "reflection_blocked";
    case reasoning_error_code::planning_failed:
      return "planning_failed";
    case reasoning_error_code::invalid_configuration:
      return "invalid_configuration";
    case reasoning_error_code::transport_failed:
      return "transport_failed";
    case reasoning_error_code::unknown:
      return "unknown";
  }
  return "unknown";
}

inline const std::error_category& reasoning_error_category() noexcept {
  class category final : public std::error_category {
  public:
    const char* name() const noexcept override {
      return "wuwe.reasoning";
    }

    std::string message(int value) const override {
      return to_string(static_cast<reasoning_error_code>(value));
    }
  };

  static category instance;
  return instance;
}

[[nodiscard]] inline std::error_code make_error_code(reasoning_error_code code) noexcept {
  return { static_cast<int>(code), reasoning_error_category() };
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

struct reasoning_task_description {
  reasoning_task_profile profile { reasoning_task_profile::auto_select };
  std::string input;
  bool has_tools { false };
  bool requires_tools { false };
  bool requires_plan { false };
  bool high_confidence_required { false };
  bool high_risk { false };
  std::map<std::string, std::string> metadata;
};

inline reasoning_policy select_policy(reasoning_task_profile profile) {
  reasoning_policy policy;
  switch (profile) {
    case reasoning_task_profile::simple_answer:
      policy.mode = reasoning_mode::simple;
      policy.enable_streaming = true;
      break;
    case reasoning_task_profile::tool_required:
      policy.mode = reasoning_mode::react;
      policy.enable_streaming = true;
      break;
    case reasoning_task_profile::complex_analysis:
    case reasoning_task_profile::plan_required:
      policy.mode = reasoning_mode::plan_execute;
      policy.allow_replanning = true;
      policy.enable_streaming = true;
      break;
    case reasoning_task_profile::high_confidence_answer:
      policy.mode = reasoning_mode::reflect_and_retry;
      policy.enable_reflection = true;
      policy.enable_streaming = true;
      break;
    case reasoning_task_profile::auto_select:
      policy.mode = reasoning_mode::react;
      policy.enable_streaming = true;
      break;
  }
  return policy;
}

inline bool contains_case_insensitive(std::string_view value, std::string_view needle) {
  if (needle.empty()) {
    return true;
  }
  if (value.size() < needle.size()) {
    return false;
  }
  for (std::size_t index = 0; index + needle.size() <= value.size(); ++index) {
    bool match = true;
    for (std::size_t offset = 0; offset < needle.size(); ++offset) {
      const auto lhs = static_cast<unsigned char>(value[index + offset]);
      const auto rhs = static_cast<unsigned char>(needle[offset]);
      if (std::tolower(lhs) != std::tolower(rhs)) {
        match = false;
        break;
      }
    }
    if (match) {
      return true;
    }
  }
  return false;
}

inline reasoning_policy select_policy(const reasoning_task_description& task) {
  if (task.profile != reasoning_task_profile::auto_select) {
    return select_policy(task.profile);
  }
  if (task.requires_plan ||
      contains_case_insensitive(task.input, "multi-step") ||
      contains_case_insensitive(task.input, "plan") ||
      contains_case_insensitive(task.input, "workflow")) {
    return select_policy(reasoning_task_profile::plan_required);
  }
  if (task.high_confidence_required || task.high_risk) {
    return select_policy(reasoning_task_profile::high_confidence_answer);
  }
  if (task.requires_tools || task.has_tools) {
    return select_policy(reasoning_task_profile::tool_required);
  }
  return select_policy(reasoning_task_profile::simple_answer);
}

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
  reasoning_error_code reasoning_error { reasoning_error_code::none };
  std::error_code underlying_error;
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

struct reasoning_error {
  reasoning_error_code code { reasoning_error_code::none };
  std::error_code underlying_error;
  std::string message;
};

struct reasoning_callbacks {
  reasoning_observer on_event;
  std::function<void(std::string_view)> on_delta;
  std::function<void(const reasoning_result&)> on_done;
  std::function<void(const reasoning_error&)> on_error;
  std::function<void(const reasoning_result&)> on_cancelled;
};

struct reasoning_run_options {
  std::stop_token stop_token;
  reasoning_callbacks callbacks;
};

inline nlohmann::json reasoning_usage_to_json(const reasoning_usage& usage) {
  return {
    { "model_calls", usage.model_calls },
    { "tool_calls", usage.tool_calls },
    { "reflection_calls", usage.reflection_calls },
    { "plan_steps", usage.plan_steps },
  };
}

inline nlohmann::json reasoning_trace_record_to_json(const reasoning_trace_record& record) {
  return {
    { "sequence", record.sequence },
    { "type", to_string(record.type) },
    { "mode", to_string(record.mode) },
    { "step_id", record.step_id },
    { "message", record.message },
    { "delta", record.delta },
    { "error", record.error },
    { "elapsed_ms", record.elapsed.count() },
    { "metadata", record.metadata },
  };
}

inline nlohmann::json reasoning_trace_to_json(
  const std::vector<reasoning_trace_record>& trace) {
  auto output = nlohmann::json::array();
  for (const auto& record : trace) {
    output.push_back(reasoning_trace_record_to_json(record));
  }
  return output;
}

inline nlohmann::json reasoning_result_to_json(const reasoning_result& result) {
  return {
    { "mode", to_string(result.mode) },
    { "completed", result.completed },
    { "content", result.content },
    { "reasoning_error", to_string(result.reasoning_error) },
    { "underlying_error", result.underlying_error ? result.underlying_error.message() : "" },
    { "error", result.error },
    { "elapsed_ms", result.elapsed.count() },
    { "usage", reasoning_usage_to_json(result.usage) },
    { "trace", reasoning_trace_to_json(result.trace) },
  };
}

} // namespace wuwe::agent::reasoning

template<>
struct std::is_error_code_enum<wuwe::agent::reasoning::reasoning_error_code> : std::true_type {};

#endif // WUWE_AGENT_REASONING_CORE_HPP
