#ifndef WUWE_AGENT_CORE_CONVERSATION_HPP
#define WUWE_AGENT_CORE_CONVERSATION_HPP

#include <wuwe/agent/core/message.hpp>
#include <wuwe/common/wuwe_fwd.h>

WUWE_AGENT_CORE_NAMESPACE_BEGIN

class conversation {
public:
  void add(message msg) {
    messages_.push_back(std::move(msg));
  }

  template<typename InputIt>
  void append(InputIt first, InputIt last) {
    messages_.insert(messages_.end(), first, last);
  }

  const std::vector<message>& messages() const noexcept {
    return messages_;
  }

  bool empty() const noexcept {
    return messages_.empty();
  }

  std::size_t size() const noexcept {
    return messages_.size();
  }

  void clear() noexcept {
    messages_.clear();
  }

  std::vector<message> visible_messages() const {
    std::vector<message> result;
    result.reserve(messages_.size());

    for (const auto& msg : messages_) {
      if (msg.visible_to_model == message_visibility::visible) {
        result.push_back(msg);
      }
    }
    return result;
  }

  std::vector<message> last(std::size_t n) const {
    if (n >= messages_.size()) {
      return messages_;
    }
    return { messages_.end() - static_cast<std::ptrdiff_t>(n), messages_.end() };
  }

private:
  std::vector<message> messages_;
};

WUWE_AGENT_CORE_NAMESPACE_END

WUWE_NAMESPACE_BEGIN

auto make_conversation() noexcept {
  return agent::core::conversation {};
}

WUWE_NAMESPACE_END

#endif // WUWE_AGENT_CORE_CONVERSATION_HPP
