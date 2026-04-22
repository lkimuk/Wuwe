#ifndef WUWE_AGENT_FLOW_PRIMITIVES_HPP
#define WUWE_AGENT_FLOW_PRIMITIVES_HPP

#include <cstddef>
#include <exception>
#include <functional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

#include <wuwe/agent/orchestration/flow.hpp>

WUWE_NAMESPACE_BEGIN

struct identity {
  template<typename T>
  auto operator()(T&& value) const {
    return std::forward<T>(value);
  }
};

class filter_error : public std::runtime_error {
public:
  explicit filter_error(const std::string& message) : std::runtime_error(message) {
  }
};

template<typename F>
class tee_step {
public:
  explicit tee_step(F func) : func_(std::move(func)) {
  }

  template<typename T>
  auto operator()(T&& value) const {
    std::invoke(func_, value);
    return std::forward<T>(value);
  }

private:
  F func_;
};

template<typename F>
auto tee(F&& func) {
  return tee_step<std::decay_t<F>>(std::forward<F>(func));
}

template<typename Pred, typename Func>
class apply_if_step {
public:
  apply_if_step(Pred pred, Func func) : pred_(std::move(pred)), func_(std::move(func)) {
  }

  template<typename T>
  auto operator()(T&& value) const {
    if (std::invoke(pred_, value)) {
      return std::invoke(func_, std::forward<T>(value));
    }

    return std::forward<T>(value);
  }

private:
  Pred pred_;
  Func func_;
};

template<typename Pred, typename Func>
auto apply_if(Pred&& pred, Func&& func) {
  return apply_if_step<std::decay_t<Pred>, std::decay_t<Func>>(
    std::forward<Pred>(pred), std::forward<Func>(func));
}

template<typename Pred>
class filter_step {
public:
  filter_step(Pred pred, std::string message)
      : pred_(std::move(pred)), message_(std::move(message)) {
  }

  template<typename T>
  auto operator()(T&& value) const {
    if (!std::invoke(pred_, value)) {
      throw filter_error(message_);
    }

    return std::forward<T>(value);
  }

private:
  Pred pred_;
  std::string message_;
};

template<typename Pred>
auto filter(Pred&& pred, std::string message = "filter predicate rejected value") {
  return filter_step<std::decay_t<Pred>>(std::forward<Pred>(pred), std::move(message));
}

template<typename Pred, typename Then, typename Else>
class if_else_step {
public:
  if_else_step(Pred pred, Then then_func, Else else_func)
      : pred_(std::move(pred)), then_func_(std::move(then_func)), else_func_(std::move(else_func)) {
  }

  template<typename T>
  auto operator()(T&& value) const {
    if (std::invoke(pred_, value)) {
      return std::invoke(then_func_, std::forward<T>(value));
    }

    return std::invoke(else_func_, std::forward<T>(value));
  }

private:
  Pred pred_;
  Then then_func_;
  Else else_func_;
};

template<typename Pred, typename Then, typename Else>
auto if_else(Pred&& pred, Then&& then_func, Else&& else_func) {
  return if_else_step<std::decay_t<Pred>, std::decay_t<Then>, std::decay_t<Else>>(
    std::forward<Pred>(pred), std::forward<Then>(then_func), std::forward<Else>(else_func));
}

template<typename Func>
class otherwise_step {
public:
  explicit otherwise_step(Func func) : func_(std::move(func)) {
  }

  template<typename T>
  auto operator()(T&& value) const {
    return std::invoke(func_, std::forward<T>(value));
  }

private:
  Func func_;
};

template<typename Func>
auto otherwise(Func&& func) {
  return otherwise_step<std::decay_t<Func>>(std::forward<Func>(func));
}

template<typename Pred, typename Func>
class when_case {
public:
  when_case(Pred pred, Func func) : pred_(std::move(pred)), func_(std::move(func)) {
  }

  template<typename T>
  bool matches(T&& value) const {
    return std::invoke(pred_, value);
  }

  template<typename T>
  auto apply(T&& value) const {
    return std::invoke(func_, std::forward<T>(value));
  }

private:
  Pred pred_;
  Func func_;
};

template<typename Pred, typename Func>
auto when(Pred&& pred, Func&& func) {
  return when_case<std::decay_t<Pred>, std::decay_t<Func>>(
    std::forward<Pred>(pred), std::forward<Func>(func));
}

template<typename Default, typename... Cases>
class route_step {
public:
  route_step(Default default_func, Cases... cases)
      : default_(std::move(default_func)), cases_(std::move(cases)...) {
  }

  template<typename T>
  auto operator()(T&& value) const {
    return dispatch<0>(std::forward<T>(value));
  }

private:
  template<std::size_t I, typename T>
  auto dispatch(T&& value) const {
    if constexpr (I < sizeof...(Cases)) {
      const auto& c = std::get<I>(cases_);
      if (c.matches(value)) {
        return c.apply(std::forward<T>(value));
      }

      return dispatch<I + 1>(std::forward<T>(value));
    }
    else {
      return std::invoke(default_, std::forward<T>(value));
    }
  }

private:
  Default default_;
  std::tuple<Cases...> cases_;
};

template<typename T>
struct is_otherwise_step : std::false_type {};

template<typename Default>
struct is_otherwise_step<otherwise_step<Default>> : std::true_type {};

template<typename T>
inline constexpr bool is_otherwise_step_v = is_otherwise_step<T>::value;

template<typename... Cases>
auto route(Cases&&... cases) {
  constexpr auto last_index = sizeof...(Cases) - 1;
  static_assert(
    is_otherwise_step_v<std::decay_t<std::tuple_element_t<last_index, std::tuple<Cases...>>>>,
    "last argument must be otherwise_step");

  auto tuple = std::make_tuple(std::forward<Cases>(cases)...);
  return [&]<std::size_t... Is>(std::index_sequence<Is...>) {
    return route_step(std::get<last_index>(tuple), std::get<Is>(tuple)...);
  }(std::make_index_sequence<last_index> {});
}

template<typename Func>
class recover_step {
public:
  explicit recover_step(Func func) : func_(std::move(func)) {
  }

  decltype(auto) recover_current_exception() const {
    try {
      std::rethrow_exception(std::current_exception());
    }
    catch (const std::exception& e) {
      if constexpr (std::is_invocable_v<const Func&, const std::exception&>) {
        return std::invoke(func_, e);
      }
      else if constexpr (std::is_invocable_v<const Func&, std::exception_ptr>) {
        return std::invoke(func_, std::current_exception());
      }
      else {
        static_assert(std::is_invocable_v<const Func&, const std::exception&> ||
                        std::is_invocable_v<const Func&, std::exception_ptr>,
          "recover handler must accept either const std::exception& or std::exception_ptr");
      }
    }
    catch (...) {
      if constexpr (std::is_invocable_v<const Func&, std::exception_ptr>) {
        return std::invoke(func_, std::current_exception());
      }
      else {
        throw;
      }
    }
  }

private:
  Func func_;
};

template<typename Func>
auto recover(Func&& func) {
  return recover_step<std::decay_t<Func>>(std::forward<Func>(func));
}

template<typename Pred>
class retry_if_step {
public:
  retry_if_step(Pred pred, std::size_t retries) : pred_(std::move(pred)), retries_(retries) {
  }

  template<typename T>
  bool should_retry(const T& value) const {
    return std::invoke(pred_, value);
  }

  std::size_t max_retries() const {
    return retries_;
  }

private:
  Pred pred_;
  std::size_t retries_;
};

template<typename Pred>
auto retry_if(Pred&& pred, std::size_t retries) {
  return retry_if_step<std::decay_t<Pred>>(std::forward<Pred>(pred), retries);
}

WUWE_NAMESPACE_END

#endif // WUWE_AGENT_FLOW_PRIMITIVES_HPP
