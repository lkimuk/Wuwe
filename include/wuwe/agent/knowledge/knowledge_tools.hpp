#ifndef WUWE_AGENT_KNOWLEDGE_TOOLS_HPP
#define WUWE_AGENT_KNOWLEDGE_TOOLS_HPP

#include <optional>
#include <algorithm>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <wuwe/agent/knowledge/knowledge_retriever.hpp>
#include <wuwe/agent/tools/tool.hpp>

namespace wuwe::agent::knowledge {

struct knowledge_tool_options {
  std::size_t max_search_results { 6 };
  double minimum_score { 0.0 };
};

struct knowledge_tool_context {
  knowledge_retriever& retriever;
  const knowledge_tool_options& options;
};

struct search_knowledge {
  static constexpr std::string_view name() noexcept {
    return "search_knowledge";
  }

  static constexpr std::string_view description =
    "Search the external knowledge base for cited document chunks relevant to the query.";

  std::string content;
  std::optional<std::string> topic;
  std::optional<std::map<std::string, std::string>> filters;
  std::optional<std::string> tenant_id;
  std::optional<std::string> user_id;
  std::optional<std::vector<std::string>> roles;
  std::optional<int> limit;
  std::optional<int> candidate_limit;

  llm_tool_result invoke(const knowledge_tool_context& context) const {
    if (content.empty()) {
      return {
        .content = "content must not be empty",
        .error_code = std::make_error_code(std::errc::invalid_argument),
      };
    }

    knowledge_query query;
    query.text = content;
    query.limit = search_limit(context);
    if (candidate_limit && *candidate_limit > 0) {
      query.candidate_limit = static_cast<std::size_t>(*candidate_limit);
    }
    query.minimum_score = context.options.minimum_score;
    if (topic && !topic->empty()) {
      query.filters["topic"] = *topic;
    }
    if (filters) {
      for (const auto& [key, value] : *filters) {
        if (!key.empty()) {
          query.filters[key] = value;
        }
      }
    }
    if (tenant_id) {
      query.access.tenant_id = *tenant_id;
    }
    if (user_id) {
      query.access.user_id = *user_id;
    }
    if (roles) {
      query.access.roles = *roles;
    }

    nlohmann::json output = nlohmann::json::array();
    for (const auto& result : context.retriever.retrieve(query)) {
      nlohmann::json item {
        { "id", result.chunk.id },
        { "document_id", result.chunk.document_id },
        { "title", result.chunk.title },
        { "source_uri", result.chunk.source_uri },
        { "content", result.chunk.content },
        { "score", result.score },
        { "vector_score", result.vector_score },
        { "lexical_score", result.lexical_score },
        { "start_line", result.chunk.start_line },
        { "end_line", result.chunk.end_line },
        { "metadata", result.chunk.metadata },
      };
      output.push_back(std::move(item));
    }

    return { .content = output.dump() };
  }

private:
  std::size_t search_limit(const knowledge_tool_context& context) const {
    const auto max_limit = context.options.max_search_results == 0
                             ? std::size_t { 1 }
                             : context.options.max_search_results;
    if (!limit) {
      return max_limit;
    }
    if (*limit <= 0) {
      return 1;
    }
    return (std::min<std::size_t>)(static_cast<std::size_t>(*limit), max_limit);
  }
};

namespace detail {

template<typename Tool>
llm_tool make_knowledge_tool() {
  return {
    .name = std::string(Tool::name()),
    .description = std::string(Tool::description),
    .parameters_json_schema =
      ::wuwe::detail::dump_json_compact(::wuwe::detail::build_object_json_schema<Tool>()),
  };
}

template<typename Tool>
llm_tool_result invoke_knowledge_tool(
  const std::string& arguments_json,
  const knowledge_tool_context& context) {
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

class knowledge_tool_provider {
public:
  explicit knowledge_tool_provider(
    knowledge_retriever& retriever,
    knowledge_tool_options options = {})
      : retriever_(retriever), options_(std::move(options)) {
  }

  std::vector<llm_tool> tools() const {
    return { detail::make_knowledge_tool<search_knowledge>() };
  }

  llm_tool_result invoke(const std::string& name, const std::string& arguments_json) const {
    const knowledge_tool_context context {
      .retriever = retriever_,
      .options = options_,
    };

    if (name == search_knowledge::name()) {
      return detail::invoke_knowledge_tool<search_knowledge>(arguments_json, context);
    }

    return {
      .content = "tool not found: " + name + ". Available tools: search_knowledge",
      .error_code = std::make_error_code(std::errc::function_not_supported),
    };
  }

private:
  knowledge_retriever& retriever_;
  knowledge_tool_options options_;
};

} // namespace wuwe::agent::knowledge

#endif // WUWE_AGENT_KNOWLEDGE_TOOLS_HPP
