#ifndef WUWE_AGENT_MEMORY_TOOLS_HPP
#define WUWE_AGENT_MEMORY_TOOLS_HPP

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <wuwe/agent/memory/memory_context.hpp>
#include <wuwe/agent/tools/tool.hpp>

namespace wuwe::agent::memory {

struct memory_tool_options {
  memory_scope scope;
  bool reject_secret_memory { true };
  std::size_t max_search_results { 8 };
  std::function<bool(const memory_record&, std::string&)> review_save;
};

struct memory_tool_context {
  memory_context& memory;
  const memory_tool_options& options;
};

struct save_memory {
  static constexpr std::string_view name() noexcept {
    return "save_memory";
  }

  static constexpr std::string_view description =
    "Save a durable long-term memory record when the user explicitly asks to remember "
    "a stable preference, project fact, or reusable context.";

  std::string content;
  std::optional<std::string> topic;
  std::optional<std::string> sensitivity;
  std::optional<std::int64_t> expires_at_unix_millis;

  llm_tool_result invoke(const memory_tool_context& context) const {
    if (content.empty()) {
      return invalid("content must not be empty");
    }

    std::map<std::string, std::string> metadata {
      { "source", "tool" },
    };

    if (topic && !topic->empty()) {
      metadata["topic"] = *topic;
    }

    if (sensitivity && !sensitivity->empty()) {
      metadata["sensitivity"] = *sensitivity;
      if (context.options.reject_secret_memory && *sensitivity == "secret") {
        return rejected("refusing to save memory marked as secret");
      }
    }

    memory_record record;
    record.kind = memory_kind::long_term;
    record.content = content;
    record.scope = active_scope(context);
    record.metadata = std::move(metadata);

    if (expires_at_unix_millis) {
      record.expires_at =
        std::chrono::system_clock::time_point(std::chrono::milliseconds(*expires_at_unix_millis));
    }

    if (context.options.review_save) {
      std::string reason;
      if (!context.options.review_save(record, reason)) {
        return rejected(reason.empty() ? "memory save rejected by review callback" : reason);
      }
    }

    const auto saved = context.memory.remember(std::move(record));
    nlohmann::json output {
      { "status", "saved" },
      { "id", saved.id },
      { "kind", to_string(saved.kind) },
    };
    return { .content = output.dump() };
  }

private:
  static memory_scope active_scope(const memory_tool_context& context) {
    if (detail::scope_has_anchor(context.options.scope)) {
      return context.options.scope;
    }
    return context.memory.scope();
  }

  static llm_tool_result invalid(std::string message) {
    return {
      .content = std::move(message),
      .error_code = std::make_error_code(std::errc::invalid_argument),
    };
  }

  static llm_tool_result rejected(std::string message) {
    return {
      .content = std::move(message),
      .error_code = std::make_error_code(std::errc::permission_denied),
    };
  }
};

struct search_memory {
  static constexpr std::string_view name() noexcept {
    return "search_memory";
  }

  static constexpr std::string_view description =
    "Search durable long-term memory records in the current application scope.";

  std::string content;
  std::optional<std::string> topic;
  std::optional<int> limit;

  llm_tool_result invoke(const memory_tool_context& context) const {
    if (content.empty()) {
      return invalid("content must not be empty");
    }

    memory_query memory_query;
    memory_query.text = content;
    memory_query.scope = active_scope(context);
    memory_query.kinds = { memory_kind::long_term };
    memory_query.limit = search_limit(context);

    if (topic && !topic->empty()) {
      memory_query.filters["topic"] = *topic;
    }

    nlohmann::json output = nlohmann::json::array();
    for (const auto& record : context.memory.recall(memory_query)) {
      if (!detail::memory_record_visible_to_model(record)) {
        continue;
      }

      nlohmann::json item {
        { "id", record.id },
        { "kind", to_string(record.kind) },
        { "content", record.summary.empty() ? record.content : record.summary },
        { "metadata", record.metadata },
      };
      output.push_back(std::move(item));
    }

    return { .content = output.dump() };
  }

private:
  static memory_scope active_scope(const memory_tool_context& context) {
    if (detail::scope_has_anchor(context.options.scope)) {
      return context.options.scope;
    }
    return context.memory.scope();
  }

  std::size_t search_limit(const memory_tool_context& context) const {
    const auto policy_limit = context.memory.policy().max_long_term_records;
    const auto tool_limit =
      context.options.max_search_results == 0 ? policy_limit : context.options.max_search_results;
    const auto max_limit = (std::max<std::size_t>)(1, (std::min)(policy_limit, tool_limit));

    if (!limit) {
      return max_limit;
    }
    if (*limit <= 0) {
      return 1;
    }
    return (std::min<std::size_t>)(static_cast<std::size_t>(*limit), max_limit);
  }

  static llm_tool_result invalid(std::string message) {
    return {
      .content = std::move(message),
      .error_code = std::make_error_code(std::errc::invalid_argument),
    };
  }
};

namespace detail {

template<typename Tool>
llm_tool make_memory_tool() {
  return {
    .name = std::string(Tool::name()),
    .description = std::string(Tool::description),
    .parameters_json_schema =
      ::wuwe::detail::dump_json_compact(::wuwe::detail::build_object_json_schema<Tool>()),
  };
}

template<typename Tool>
llm_tool_result invoke_memory_tool(
  const std::string& arguments_json,
  const memory_tool_context& context) {
  try {
    const auto args = nlohmann::json::parse(arguments_json.empty() ? "{}" : arguments_json);
    const auto tool = ::wuwe::detail::tool_json_get<Tool>(args);
    return tool.invoke(context);
  }
  catch (const std::exception& ex) {
    return {
      .content = std::string("invalid arguments for tool '") +
                 std::string(Tool::name()) + "': " + ex.what(),
      .error_code = std::make_error_code(std::errc::invalid_argument),
    };
  }
}

} // namespace detail

class memory_tool_provider {
public:
  explicit memory_tool_provider(memory_context& memory, memory_tool_options options = {})
      : memory_(memory), options_(std::move(options)) {
  }

  std::vector<llm_tool> tools() const {
    return {
      detail::make_memory_tool<save_memory>(),
      detail::make_memory_tool<search_memory>(),
    };
  }

  llm_tool_result invoke(const std::string& name, const std::string& arguments_json) const {
    const memory_tool_context context { .memory = memory_, .options = options_ };

    if (name == save_memory::name()) {
      return detail::invoke_memory_tool<save_memory>(arguments_json, context);
    }
    if (name == search_memory::name()) {
      return detail::invoke_memory_tool<search_memory>(arguments_json, context);
    }

    return {
      .content = "tool not found: " + name + ". Available tools: save_memory, search_memory",
      .error_code = std::make_error_code(std::errc::function_not_supported),
    };
  }

private:
  memory_context& memory_;
  memory_tool_options options_;
};

} // namespace wuwe::agent::memory

#endif // WUWE_AGENT_MEMORY_TOOLS_HPP
