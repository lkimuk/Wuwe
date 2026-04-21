#ifndef WUWE_AGENT_FLOW_HPP
#define WUWE_AGENT_FLOW_HPP

#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <wuwe/agent/llm/llm_client.h>

WUWE_NAMESPACE_BEGIN

struct flow_context {};

template<typename Func>
class recover_step;

template<typename Pred>
class retry_if_step;

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
inline constexpr bool is_recover_step_v = false;

template<typename T>
inline constexpr bool is_retry_if_step_v = false;

template<typename Func>
inline constexpr bool is_recover_step_v<recover_step<Func>> = true;

template<typename Pred>
inline constexpr bool is_retry_if_step_v<retry_if_step<Pred>> = true;

template<typename T>
auto continue_flow(const std::shared_ptr<llm_client>& client, T&& value) {
  if constexpr (is_prompt_text_v<T>) {
    return client->complete(std::string_view(value));
  }
  else if constexpr (is_llm_request_v<T>) {
    return client->complete(std::forward<T>(value));
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
  auto then(G&& g) const& {
    return compose_next(callable_, std::forward<G>(g));
  }

  template<typename G>
  auto then(G&& g) && {
    return compose_next(std::move(callable_), std::forward<G>(g));
  }

private:
  template<typename Prev, typename G>
  auto compose_next(Prev&& prev, G&& g) const {
    using g_type = std::decay_t<G>;

    if constexpr (detail::is_recover_step_v<g_type>) {
      auto composed = [prev = std::forward<Prev>(prev), g = std::forward<G>(g)](
                        const std::shared_ptr<llm_client>& client, auto&& x) -> decltype(auto) {
        try {
          return detail::continue_flow(
            client, std::invoke(prev, client, std::forward<decltype(x)>(x)));
        }
        catch (...) {
          return detail::continue_flow(client, g.recover_current_exception());
        }
      };

      return flow<std::decay_t<decltype(composed)>>(client_, std::move(composed));
    }
    else if constexpr (detail::is_retry_if_step_v<g_type>) {
      auto composed = [prev = std::forward<Prev>(prev), g = std::forward<G>(g)](
                        const std::shared_ptr<llm_client>& client, auto&& x) -> decltype(auto) {
        using input_type = detail::decay_t<decltype(x)>;

        static_assert(std::is_copy_constructible_v<input_type>,
          "retry_if requires the previous step input to be copy constructible");

        input_type stable_input(std::forward<decltype(x)>(x));

        for (std::size_t attempt = 0;; ++attempt) {
          input_type attempt_input = stable_input;
          auto result = std::invoke(prev, client, attempt_input);

          if (!g.should_retry(result) || attempt >= g.max_retries()) {
            return detail::continue_flow(client, std::move(result));
          }
        }
      };

      return flow<std::decay_t<decltype(composed)>>(client_, std::move(composed));
    }
    else {
      auto composed = [prev = std::forward<Prev>(prev), g = std::forward<G>(g)](
                        const std::shared_ptr<llm_client>& client, auto&& x) -> decltype(auto) {
        return detail::continue_flow(
          client, std::invoke(g, std::invoke(prev, client, std::forward<decltype(x)>(x))));
      };

      return flow<std::decay_t<decltype(composed)>>(client_, std::move(composed));
    }
  }

private:
  std::shared_ptr<llm_client> client_;
  F callable_;
};

template<typename F, typename G>
auto operator|(const flow<F>& f, G&& g) {
  return f.then(std::forward<G>(g));
}

template<typename F, typename G>
auto operator|(flow<F>&& f, G&& g) {
  return std::move(f).then(std::forward<G>(g));
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
