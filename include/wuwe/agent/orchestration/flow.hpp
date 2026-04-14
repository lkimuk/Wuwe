#ifndef WUWE_AGENT_FLOW_HPP
#define WUWE_AGENT_FLOW_HPP

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <wuwe/agent/llm/llm_client.h>

WUWE_NAMESPACE_BEGIN

namespace detail {

template<typename T>
using decay_t = typename std::decay<T>::type;

template<typename T>
inline constexpr bool is_prompt_text_v =
  std::is_same_v<decay_t<T>, std::string> || std::is_same_v<decay_t<T>, std::string_view> ||
  std::is_same_v<decay_t<T>, const char*> || std::is_same_v<decay_t<T>, char*>;

template<typename T>
inline constexpr bool is_llm_request_v = std::is_same_v<decay_t<T>, llm_request>;

template<typename T>
decltype(auto) continue_flow(const std::shared_ptr<llm_client>& client, T&& value) {
  if constexpr (is_prompt_text_v<T>) {
    return client->complete(std::string_view(value));
  }
  else if constexpr (is_llm_request_v<T>) {
    return client->complete(value);
  }
  else {
    return std::forward<T>(value);
  }
}

} // namespace detail

template<typename F>
class flow {
public:
  flow(std::shared_ptr<llm_client> client, F func)
      : client_(std::move(client)), callable_(std::move(func)) {
  }

  template<typename U>
  decltype(auto) invoke(U&& input) const {
    return std::invoke(callable_, client_, std::forward<U>(input));
  }

  template<typename G>
  auto then(G&& g) const {
    auto f = callable_;

    auto composed = [f, g = std::forward<G>(g)](
                      const std::shared_ptr<llm_client>& client, auto&& x) -> decltype(auto) {
      return detail::continue_flow(
        client, std::invoke(g, std::invoke(f, client, std::forward<decltype(x)>(x))));
    };

    return flow<std::decay_t<decltype(composed)>>(client_, std::move(composed));
  }

private:
  std::shared_ptr<llm_client> client_;
  F callable_;
};

template<typename F, typename G>
auto operator|(const flow<F>& r, G&& g) {
  return r.then(std::forward<G>(g));
}

template<typename F>
auto operator|(std::shared_ptr<llm_client> client, F&& f) {
  auto first = [func = std::forward<F>(f)](
                 const std::shared_ptr<llm_client>& client, auto&& input) -> decltype(auto) {
    return detail::continue_flow(client, std::invoke(func, std::forward<decltype(input)>(input)));
  };

  return flow<std::decay_t<decltype(first)>>(std::move(client), std::move(first));
}

WUWE_NAMESPACE_END

#endif // WUWE_AGENT_FLOW_HPP
