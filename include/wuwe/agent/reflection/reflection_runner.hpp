#ifndef WUWE_AGENT_REFLECTION_RUNNER_HPP
#define WUWE_AGENT_REFLECTION_RUNNER_HPP

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include <wuwe/agent/reflection/reflector.hpp>
#include <wuwe/agent/reflection/reflection_store.hpp>

namespace wuwe::agent::reflection {

enum class reflection_event_type {
  reflection_started,
  reflection_completed,
};

struct reflection_event {
  reflection_event_type type;
  const reflection_request* request {};
  const reflection_result* result {};
  std::string record_id;
  std::chrono::milliseconds elapsed { 0 };
};

using reflection_observer = std::function<void(const reflection_event&)>;

struct reflection_runner_options {
  std::shared_ptr<::wuwe::agent::reflection::reflector> reflector;
  reflection_policy policy;
  reflection_store* store {};
  reflection_observer observer;
};

struct reflection_run_result {
  reflection_result result;
  reflection_record record;
  std::chrono::milliseconds elapsed { 0 };
};

class reflection_runtime_services {
public:
  reflection_runtime_services(reflection_store* store, reflection_observer observer)
      : store_(store), observer_(std::move(observer)) {
  }

  void notify(reflection_event event) const {
    if (observer_) {
      observer_(event);
    }
  }

  void save(const reflection_record& record) const {
    if (store_) {
      store_->save(record);
    }
  }

  std::string next_id() const {
    return "reflection-" + std::to_string(next_id_++);
  }

private:
  reflection_store* store_ {};
  reflection_observer observer_;
  inline static std::size_t next_id_ { 1 };
};

class reflection_runner {
public:
  explicit reflection_runner(reflection_runner_options options) : options_(std::move(options)) {
    if (!options_.reflector) {
      throw std::invalid_argument("reflection_runner requires reflector");
    }
  }

  reflection_run_result run(reflection_request request) {
    const auto started = std::chrono::steady_clock::now();
    auto services = runtime_services();
    services.notify({ .type = reflection_event_type::reflection_started, .request = &request });

    auto result = reflection_policy_engine(options_.policy).apply(options_.reflector->reflect(request));

    reflection_record record {
      .id = services.next_id(),
      .request = std::move(request),
      .result = result,
    };

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);
    services.save(record);
    services.notify({
      .type = reflection_event_type::reflection_completed,
      .request = &record.request,
      .result = &record.result,
      .record_id = record.id,
      .elapsed = elapsed,
    });

    return {
      .result = std::move(result),
      .record = std::move(record),
      .elapsed = elapsed,
    };
  }

private:
  reflection_runtime_services runtime_services() const {
    return reflection_runtime_services(options_.store, options_.observer);
  }

  reflection_runner_options options_;
};

} // namespace wuwe::agent::reflection

#endif // WUWE_AGENT_REFLECTION_RUNNER_HPP
