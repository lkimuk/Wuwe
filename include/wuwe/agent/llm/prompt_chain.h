#ifndef WUWE_AGENT_LLM_PROMPT_CHAIN_H
#define WUWE_AGENT_LLM_PROMPT_CHAIN_H

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <wuwe/agent/llm/llm_agent_runner.h>
#include <wuwe/common/wuwe_fwd.h>

WUWE_NAMESPACE_BEGIN

using chain_json = nlohmann::json;

class chain_state {
public:
  chain_state() = default;
  explicit chain_state(std::string input) : input(std::move(input)) {}

  std::string input;

  bool contains(std::string_view key) const {
    return values_.contains(std::string(key));
  }

  const chain_json& get_json(std::string_view key) const {
    return values_.at(std::string(key));
  }

  chain_json& get_json(std::string_view key) {
    return values_[std::string(key)];
  }

  template <typename T>
  T get(std::string_view key) const {
    return get_json(key).template get<T>();
  }

  template <typename T>
  void set(std::string_view key, T&& value) {
    values_[std::string(key)] = std::forward<T>(value);
  }

  void erase(std::string_view key) {
    values_.erase(std::string(key));
  }

  const chain_json& values() const {
    return values_;
  }

  chain_json& values() {
    return values_;
  }

private:
  chain_json values_ = chain_json::object();
};

struct chain_step_result {
  bool ok { true };
  bool done { false };
  std::optional<std::string> next_step;
  std::error_code error_code;
  std::string error_message;

  static chain_step_result success(std::optional<std::string> next_step = std::nullopt) {
    chain_step_result result;
    result.next_step = std::move(next_step);
    return result;
  }

  static chain_step_result finish() {
    chain_step_result result;
    result.done = true;
    return result;
  }

  static chain_step_result failure(std::error_code error_code, std::string error_message = {}) {
    chain_step_result result;
    result.ok = false;
    result.error_code = error_code;
    result.error_message = std::move(error_message);
    return result;
  }
};

struct chain_run_result {
  chain_state state;
  std::string output;
  std::error_code error_code;
  std::string error_message;

  explicit operator bool() const noexcept {
    return !error_code;
  }
};

class prompt_chain_step {
public:
  virtual ~prompt_chain_step() = default;

  virtual std::string_view name() const = 0;
  virtual chain_step_result run(llm_client& client, chain_state& state) = 0;
};

namespace detail {

inline chain_json parse_text_as_json_or_string(const std::string& text) {
  const auto parsed = chain_json::parse(text, nullptr, false);
  return parsed.is_discarded() ? chain_json(text) : parsed;
}

inline std::string default_prompt_from_state(const chain_state& state) {
  return state.input;
}

inline std::string default_output_from_state(const chain_state& state) {
  if (state.contains("final_output")) {
    const auto& final_output = state.get_json("final_output");
    return final_output.is_string() ? final_output.get<std::string>() : final_output.dump();
  }

  if (state.contains("__last_response__")) {
    const auto& last_response = state.get_json("__last_response__");
    return last_response.is_string() ? last_response.get<std::string>() : last_response.dump();
  }

  return {};
}

} // namespace detail

struct llm_step_config {
  std::string name;
  std::string system_prompt;
  std::function<std::string(const chain_state&)> prompt = detail::default_prompt_from_state;
  std::optional<std::string> output_schema_json;
  std::optional<std::string> state_key;
  std::optional<std::string> next_step;
  std::shared_ptr<const llm_tool_provider> tool_provider;
  int max_tool_rounds { 4 };
  std::function<void(chain_state&, const llm_response&)> apply_response;
};

class llm_step final : public prompt_chain_step {
public:
  explicit llm_step(llm_step_config config) : config_(std::move(config)) {}

  std::string_view name() const override {
    return config_.name;
  }

  chain_step_result run(llm_client& client, chain_state& state) override {
    llm_request request;
    if (!config_.system_prompt.empty()) {
      request.messages.push_back({ .role = "system", .content = config_.system_prompt });
    }
    request.messages.push_back({ .role = "user", .content = config_.prompt(state) });
    request.response_format = config_.output_schema_json;

    const llm_response response =
      detail::complete_request_with_tools(client, std::move(request), config_.tool_provider, config_.max_tool_rounds);
    if (response.error_code) {
      return chain_step_result::failure(response.error_code, response.error_code.message());
    }

    state.set("__last_response__", response.content);
    if (config_.state_key.has_value()) {
      state.set(*config_.state_key, detail::parse_text_as_json_or_string(response.content));
    }
    if (config_.apply_response) {
      config_.apply_response(state, response);
    }

    return chain_step_result::success(config_.next_step);
  }

private:
  llm_step_config config_;
};

struct tool_step_config {
  std::string name;
  std::function<llm_tool_result(chain_state&)> invoke;
  std::shared_ptr<const llm_tool_provider> tool_provider;
  std::string tool_name;
  std::function<chain_json(const chain_state&)> args_from_state;
  std::optional<std::string> result_key;
  std::optional<std::string> next_step;
};

class tool_step final : public prompt_chain_step {
public:
  explicit tool_step(tool_step_config config) : config_(std::move(config)) {}

  std::string_view name() const override {
    return config_.name;
  }

  chain_step_result run(llm_client&, chain_state& state) override {
    llm_tool_result result;

    if (config_.invoke) {
      result = config_.invoke(state);
    }
    else {
      if (config_.tool_provider == nullptr) {
        return chain_step_result::failure(
          std::make_error_code(std::errc::function_not_supported), "tool_step has no tool provider");
      }

      if (config_.tool_name.empty()) {
        return chain_step_result::failure(
          std::make_error_code(std::errc::invalid_argument), "tool_step is missing tool_name");
      }

      const chain_json args = config_.args_from_state ? config_.args_from_state(state) : chain_json::object();
      result = config_.tool_provider->invoke(config_.tool_name, args.dump());
    }

    if (config_.result_key.has_value()) {
      state.set(*config_.result_key, detail::parse_text_as_json_or_string(result.content));
    }

    if (result.error_code) {
      const std::string message = result.content.empty() ? result.error_code.message() : result.content;
      return chain_step_result::failure(result.error_code, message);
    }

    return chain_step_result::success(config_.next_step);
  }

private:
  tool_step_config config_;
};

struct decision_step_config {
  std::string name;
  std::function<chain_step_result(const chain_state&)> decide;
};

class decision_step final : public prompt_chain_step {
public:
  explicit decision_step(decision_step_config config) : config_(std::move(config)) {}

  std::string_view name() const override {
    return config_.name;
  }

  chain_step_result run(llm_client&, chain_state& state) override {
    return config_.decide(state);
  }

private:
  decision_step_config config_;
};

class prompt_chain {
public:
  prompt_chain& add_step(std::unique_ptr<prompt_chain_step> step) {
    const std::string name(step->name());
    if (steps_.contains(name)) {
      throw std::invalid_argument("duplicate prompt chain step: " + name);
    }

    order_.push_back(name);
    steps_.emplace(name, std::move(step));
    return *this;
  }

  prompt_chain& set_start(std::string step_name) {
    start_step_ = std::move(step_name);
    return *this;
  }

  prompt_chain& set_max_steps(int value) {
    max_steps_ = value;
    return *this;
  }

  chain_run_result run(llm_client& client, chain_state initial_state) const {
    chain_run_result result;
    result.state = std::move(initial_state);

    if (order_.empty()) {
      result.error_code = std::make_error_code(std::errc::invalid_argument);
      result.error_message = "prompt chain has no steps";
      return result;
    }

    std::string current_step = start_step_.empty() ? order_.front() : start_step_;

    for (int executed = 0; executed < max_steps_; ++executed) {
      const auto it = steps_.find(current_step);
      if (it == steps_.end()) {
        result.error_code = std::make_error_code(std::errc::invalid_argument);
        result.error_message = "unknown prompt chain step: " + current_step;
        return result;
      }

      const chain_step_result step_result = it->second->run(client, result.state);
      if (!step_result.ok) {
        result.error_code = step_result.error_code ? step_result.error_code
                                                   : std::make_error_code(std::errc::operation_canceled);
        result.error_message = step_result.error_message;
        return result;
      }

      if (step_result.done) {
        result.output = detail::default_output_from_state(result.state);
        return result;
      }

      if (step_result.next_step.has_value()) {
        if (*step_result.next_step == "__finish__") {
          result.output = detail::default_output_from_state(result.state);
          return result;
        }
        current_step = *step_result.next_step;
        continue;
      }

      const auto order_it = std::find(order_.begin(), order_.end(), current_step);
      if (order_it == order_.end() || std::next(order_it) == order_.end()) {
        result.output = detail::default_output_from_state(result.state);
        return result;
      }

      current_step = *std::next(order_it);
    }

    result.error_code = std::make_error_code(std::errc::resource_unavailable_try_again);
    result.error_message = "prompt chain exceeded maximum step count";
    return result;
  }

private:
  std::unordered_map<std::string, std::unique_ptr<prompt_chain_step>> steps_;
  std::vector<std::string> order_;
  std::string start_step_;
  int max_steps_ { 16 };
};

class prompt_chain_builder {
public:
  prompt_chain_builder& llm(std::string name, llm_step_config config) {
    config.name = std::move(name);
    chain_.add_step(std::make_unique<llm_step>(std::move(config)));
    return *this;
  }

  prompt_chain_builder& tool(std::string name, tool_step_config config) {
    config.name = std::move(name);
    chain_.add_step(std::make_unique<tool_step>(std::move(config)));
    return *this;
  }

  prompt_chain_builder& decision(std::string name, decision_step_config config) {
    config.name = std::move(name);
    chain_.add_step(std::make_unique<decision_step>(std::move(config)));
    return *this;
  }

  prompt_chain_builder& decision(
    std::string name, std::function<chain_step_result(const chain_state&)> decide) {
    return decision(std::move(name), decision_step_config { .decide = std::move(decide) });
  }

  prompt_chain_builder& set_max_steps(int value) {
    chain_.set_max_steps(value);
    return *this;
  }

  prompt_chain build(std::string start_step) & {
    chain_.set_start(std::move(start_step));
    return std::move(chain_);
  }

  prompt_chain build(std::string start_step) && {
    chain_.set_start(std::move(start_step));
    return std::move(chain_);
  }

private:
  prompt_chain chain_;
};

WUWE_NAMESPACE_END

#endif // WUWE_AGENT_LLM_PROMPT_CHAIN_H
