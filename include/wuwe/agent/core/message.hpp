#ifndef WUWE_AGENT_MESSAGE_H
#define WUWE_AGENT_MESSAGE_H

#include <concepts>
#include <cstddef>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <wuwe/agent/llm/llm_types.h>
#include <wuwe/common/wuwe_fwd.h>

WUWE_AGENT_CORE_NAMESPACE_BEGIN

enum class message_role {
  system,
  user,
  assistant,
  tool,
};

enum class message_visibility {
  visible,
  hidden,
};

struct message {
  message_role role;
  std::string content;

  std::optional<std::string> id;
  std::optional<std::string> name;

  message_visibility visible_to_model = message_visibility::visible;

  std::map<std::string, std::string> metadata;
};

WUWE_AGENT_CORE_NAMESPACE_END

WUWE_NAMESPACE_BEGIN

inline namespace literals {
inline namespace message_literals {

namespace msg_literals_detail {
struct message_maker {
  std::string role;

  template<typename T>
    requires std::convertible_to<T, std::string_view>
  chat_message operator()(T&& content) const {
    return {
      .role = role,
      .content = std::string(std::string_view(std::forward<T>(content))),
    };
  }
};
} // namespace msg_literals_detail

inline msg_literals_detail::message_maker operator""_msg(const char* role, std::size_t length) {
  const std::string_view role_view(role, length);
  if (gmp::enum_cast<agent::core::message_role>(role_view)) {
    return { std::string(role_view) };
  }

  throw std::invalid_argument("invalid message role");
}

} // namespace message_literals
} // namespace literals

inline llm_request make_message() {
  return {};
}

inline llm_request operator<<(llm_request&& request, chat_message message) {
  request.messages.push_back(std::move(message));
  return request;
}

inline constexpr auto says =
  gmp::make_named_operator([](std::string_view role, const std::string& content) -> chat_message {
    return {
      .role = std::string(role),
      .content = content,
    };
  });

WUWE_NAMESPACE_END

#endif // WUWE_AGENT_MESSAGE_H
