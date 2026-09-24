#ifndef WUWE_AGENT_LLM_CONTEXT_TOKEN_ESTIMATOR_HPP
#define WUWE_AGENT_LLM_CONTEXT_TOKEN_ESTIMATOR_HPP

#include <algorithm>
#include <cstddef>
#include <limits>
#include <string>
#include <string_view>

#include <wuwe/agent/llm/llm_types.h>
#include <wuwe/agent/llm/text_token_estimator.hpp>

namespace wuwe::agent::llm {

namespace detail {

inline void saturating_context_token_add(std::size_t& target, std::size_t value) noexcept {
  const auto maximum = (std::numeric_limits<std::size_t>::max)();
  target = value > maximum - target ? maximum : target + value;
}

[[nodiscard]] inline std::size_t saturating_context_token_sum(
  std::size_t lhs, std::size_t rhs) noexcept {
  saturating_context_token_add(lhs, rhs);
  return lhs;
}

} // namespace detail

class context_token_estimator : public text_token_estimator {
public:
  ~context_token_estimator() override = default;

  [[nodiscard]] virtual std::size_t estimate_message(const ::wuwe::chat_message& message) const {
    std::size_t tokens = 4;
    detail::saturating_context_token_add(tokens, estimate_text(message.role));
    detail::saturating_context_token_add(tokens, estimate_text(message.content));
    detail::saturating_context_token_add(tokens, estimate_text(message.reasoning_content));
    if (message.provider_state) {
      // Conservative wire-size estimate; never assume opaque state is free.
      detail::saturating_context_token_add(tokens, estimate_text(message.provider_state->data));
    }
    if (message.name) {
      detail::saturating_context_token_add(tokens, estimate_text(*message.name));
    }
    if (message.tool_call_id) {
      detail::saturating_context_token_add(tokens, estimate_text(*message.tool_call_id));
    }
    for (const auto& call : message.tool_calls) {
      detail::saturating_context_token_add(tokens, 4);
      detail::saturating_context_token_add(tokens, estimate_text(call.id));
      detail::saturating_context_token_add(tokens, estimate_text(call.name));
      detail::saturating_context_token_add(tokens, estimate_text(call.arguments_json));
    }
    return tokens;
  }

  [[nodiscard]] virtual std::size_t estimate_tool(const ::wuwe::llm_tool& tool) const {
    std::size_t tokens = 8;
    detail::saturating_context_token_add(tokens, estimate_text(tool.name));
    detail::saturating_context_token_add(tokens, estimate_text(tool.description));
    detail::saturating_context_token_add(tokens, estimate_text(tool.parameters_json_schema));
    return tokens;
  }
};

class heuristic_context_token_estimator final : public context_token_estimator {
public:
  [[nodiscard]] std::size_t estimate_text(std::string_view text) const override {
    std::size_t ascii = 0;
    std::size_t non_ascii_codepoints = 0;
    for (std::size_t index = 0; index < text.size();) {
      const auto byte = static_cast<unsigned char>(text[index]);
      if (byte < 0x80) {
        ++ascii;
        ++index;
      }
      else {
        ++non_ascii_codepoints;
        if ((byte & 0xe0) == 0xc0)
          index += 2;
        else if ((byte & 0xf0) == 0xe0)
          index += 3;
        else if ((byte & 0xf8) == 0xf0)
          index += 4;
        else
          ++index;
        index = (std::min)(index, text.size());
      }
    }
    return (ascii + 3) / 4 + non_ascii_codepoints;
  }

  [[nodiscard]] std::string truncate_text(
    std::string_view text, std::size_t token_limit, bool keep_tail) const override {
    if (estimate_text(text) <= token_limit)
      return std::string(text);
    if (token_limit == 0)
      return {};
    std::size_t low = 0;
    std::size_t high = text.size();
    while (low < high) {
      const auto length = low + (high - low + 1) / 2;
      const auto candidate = keep_tail ? valid_tail(text, length) : valid_prefix(text, length);
      if (estimate_text(candidate) <= token_limit)
        low = length;
      else
        high = length - 1;
    }
    return std::string(keep_tail ? valid_tail(text, low) : valid_prefix(text, low));
  }

private:
  [[nodiscard]] static std::string_view valid_prefix(std::string_view text, std::size_t length) {
    length = (std::min)(length, text.size());
    while (length > 0 && length < text.size() &&
           (static_cast<unsigned char>(text[length]) & 0xc0) == 0x80) {
      --length;
    }
    return text.substr(0, length);
  }

  [[nodiscard]] static std::string_view valid_tail(std::string_view text, std::size_t length) {
    length = (std::min)(length, text.size());
    auto start = text.size() - length;
    while (start < text.size() && (static_cast<unsigned char>(text[start]) & 0xc0) == 0x80) {
      ++start;
    }
    return text.substr(start);
  }
};

} // namespace wuwe::agent::llm

#endif // WUWE_AGENT_LLM_CONTEXT_TOKEN_ESTIMATOR_HPP
