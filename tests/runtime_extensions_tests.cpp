#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <wuwe/agent/llm/context_budget.hpp>
#include <wuwe/agent/llm/llm_agent_runner.h>
#include <wuwe/agent/llm/scripted_llm_client.hpp>
#include <wuwe/agent/runtime/executor.hpp>
#include <wuwe/agent/runtime/scheduler.hpp>
#include <wuwe/agent/tools/json_schema.hpp>

namespace {

using namespace wuwe;
using namespace wuwe::agent;

void require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

class unit_estimator final : public llm::context_token_estimator {
public:
  std::size_t estimate_text(std::string_view text) const override {
    return text.size();
  }

  std::string truncate_text(
    std::string_view text, std::size_t token_limit, bool keep_tail) const override {
    token_limit = (std::min)(token_limit, text.size());
    return keep_tail ? std::string(text.substr(text.size() - token_limit))
                     : std::string(text.substr(0, token_limit));
  }
};

class cancellable_client final : public llm_client {
public:
  llm_response complete(const llm_request&) override {
    return { .content = "unexpected synchronous completion" };
  }

  llm_response complete(const llm_request&, std::stop_token stop_token) override {
    started.set_value();
    while (!stop_token.stop_requested()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    cancellation_observed = true;
    return {
      .content = "cancelled",
      .error_code = make_error_code(llm_error_code::cancelled),
    };
  }

  std::promise<void> started;
  std::atomic<bool> cancellation_observed { false };
};

class gated_client final : public llm_client {
public:
  gated_client() : release_future(release.get_future().share()) {
  }

  llm_response complete(const llm_request&) override {
    return { .content = "unexpected synchronous completion" };
  }

  llm_response complete(const llm_request&, std::stop_token) override {
    started.set_value();
    release_future.wait();
    return { .content = "done" };
  }

  std::promise<void> started;
  std::promise<void> release;
  std::shared_future<void> release_future;
};

class manual_executor final : public runtime::executor {
public:
  runtime::scheduled_task submit(runtime::executor_work work) override {
    runtime::scheduled_task_source source;
    const auto task = source.task();
    pending.emplace(std::move(work), std::move(source));
    return task;
  }

  std::size_t concurrency() const noexcept override {
    return 1;
  }

  void run_one() {
    require(pending.has_value(), "manual executor requires pending work");
    auto current = std::move(*pending);
    pending.reset();
    current.second.execute(std::move(current.first));
  }

private:
  std::optional<std::pair<runtime::executor_work, runtime::scheduled_task_source>> pending;
};

void test_context_budget() {
  llm_request request;
  request.messages = {
    { .role = "system", .content = "system-contract" },
    { .role = "user", .content = "old-memory-value", .context_source = llm_context_source::memory },
    { .role = "user",
      .content = "retrieved-knowledge",
      .context_source = llm_context_source::knowledge },
    { .role = "user", .content = "old conversation" },
    { .role = "user", .content = "latest request" },
  };
  request.tools.push_back({
    .name = "lookup",
    .description = "lookup",
    .parameters_json_schema = R"({"type":"object"})",
  });
  const llm_context_budget budget {
    .context_window_tokens = 140,
    .reserved_output_tokens = 12,
    .minimum_recent_conversation_messages = 1,
    .limits = {
      .memory = 10,
      .knowledge = 10,
      .tool_schemas = 40,
    },
  };
  llm::context_budget_manager manager(std::make_shared<unit_estimator>());
  auto result = manager.fit(request, budget);
  require(static_cast<bool>(result), "context budget should fit reducible context");
  require(result.report.after.memory <= 10 && result.report.after.knowledge <= 10,
    "context budget should enforce component limits");
  require(!result.request.messages.empty() &&
            result.request.messages.back().content.find("latest") != std::string::npos,
    "context budget must retain the newest conversation message");
  require(result.request.max_output_tokens == 12,
    "context budget should enforce its reserved output allowance");

  llm_request protected_request;
  protected_request.messages.push_back({
    .role = "system",
    .content = std::string(50, 's'),
  });
  llm::context_budget_manager protected_manager(std::make_shared<unit_estimator>());
  auto protected_result = protected_manager.fit(protected_request,
    {
      .context_window_tokens = 30,
      .reserved_output_tokens = 5,
      .overflow = llm_context_overflow_policy::reject,
    });
  require(!protected_result, "context budget must reject protected system overflow by default");

  llm_request schema_heavy;
  schema_heavy.tools.push_back({
    .name = "large",
    .description = std::string(30, 'x'),
    .parameters_json_schema = R"({"type":"object"})",
  });
  auto schema_result = manager.fit(schema_heavy,
    {
      .context_window_tokens = 100,
      .reserved_output_tokens = 10,
      .limits = { .tool_schemas = 5 },
    });
  require(!schema_result, "context budget must not silently truncate tool schemas");

  llm_request tool_exchange;
  tool_exchange.messages = {
    {
      .role = "assistant",
      .tool_calls = { { .id = "tool-1", .name = "lookup", .arguments_json = "{}" } },
    },
    {
      .role = "tool",
      .content = std::string(40, 'r'),
      .tool_call_id = "tool-1",
    },
    { .role = "user", .content = "continue" },
  };
  auto exchange_result = manager.fit(tool_exchange,
    {
      .context_window_tokens = 80,
      .reserved_output_tokens = 10,
      .limits = { .tool_results = 1 },
      .minimum_recent_tool_exchanges = 0,
    });
  require(static_cast<bool>(exchange_result) && exchange_result.request.messages.size() == 1 &&
            exchange_result.request.messages.front().role == "user",
    "context budget must remove a tool exchange atomically");

  tool_exchange.messages.insert(tool_exchange.messages.begin() + 2,
    {
      .role = "assistant",
      .tool_calls = { { .id = "tool-2", .name = "lookup", .arguments_json = "{}" } },
    });
  tool_exchange.messages.insert(tool_exchange.messages.begin() + 3,
    {
      .role = "tool",
      .content = std::string(40, 's'),
      .tool_call_id = "tool-2",
    });
  auto multiple_exchange_result = manager.fit(tool_exchange,
    {
      .context_window_tokens = 80,
      .reserved_output_tokens = 10,
      .limits = { .tool_results = 1 },
      .minimum_recent_tool_exchanges = 0,
    });
  require(static_cast<bool>(multiple_exchange_result) &&
            multiple_exchange_result.report.dropped_messages == 4 &&
            multiple_exchange_result.request.messages.size() == 1,
    "multiple tool exchanges must be removed without double counting");

  const auto structured_latest = nlohmann::json({
                                                  { "ok", false },
                                                  { "error",
                                                    {
                                                      { "category", "timeout" },
                                                      { "code", 110 },
                                                      { "retryable", true },
                                                    } },
                                                })
                                   .dump();
  llm_request protected_tool_exchange;
  protected_tool_exchange.messages = {
    {
      .role = "assistant",
      .tool_calls = { { .id = "old-tool", .name = "lookup", .arguments_json = "{}" } },
    },
    {
      .role = "tool",
      .content = std::string(300, 'o'),
      .tool_call_id = "old-tool",
    },
    {
      .role = "assistant",
      .tool_calls = { { .id = "latest-tool", .name = "lookup", .arguments_json = "{}" } },
    },
    {
      .role = "tool",
      .content = structured_latest,
      .tool_call_id = "latest-tool",
    },
    { .role = "user", .content = "continue" },
  };
  auto protected_exchange_result = manager.fit(protected_tool_exchange,
    {
      .context_window_tokens = 512,
      .reserved_output_tokens = 10,
      .limits = { .tool_results = 160 },
    });
  require(
    protected_exchange_result && protected_exchange_result.report.dropped_messages == 2 &&
      protected_exchange_result.report.truncated_messages == 0 &&
      protected_exchange_result.request.messages.size() == 3 &&
      protected_exchange_result.request.messages.front().tool_calls.front().id == "latest-tool" &&
      protected_exchange_result.request.messages[1].content == structured_latest &&
      nlohmann::json::parse(protected_exchange_result.request.messages[1].content)
          .at("error")
          .at("category") == "timeout",
    "context budgeting must drop old tool exchanges atomically while preserving the newest "
    "structured projection byte-for-byte");

  llm_request retention_heavy_tool_history;
  for (int index = 0; index < 6; ++index) {
    const auto id = "retained-tool-" + std::to_string(index);
    retention_heavy_tool_history.messages.push_back({
      .role = "assistant",
      .tool_calls = { { .id = id, .name = "lookup", .arguments_json = "{}" } },
    });
    retention_heavy_tool_history.messages.push_back({
      .role = "tool",
      .content = std::string(40, static_cast<char>('a' + index)),
      .tool_call_id = id,
    });
  }
  retention_heavy_tool_history.messages.push_back(
    { .role = "user", .content = "finish with the newest evidence" });
  const auto relaxed_tool_floor_result = manager.fit(retention_heavy_tool_history,
    {
      .context_window_tokens = 512,
      .reserved_output_tokens = 10,
      .limits = { .tool_results = 150 },
      .minimum_recent_tool_exchanges = 12,
    });
  require(relaxed_tool_floor_result &&
            relaxed_tool_floor_result.report.preservation_floor_relaxed &&
            relaxed_tool_floor_result.report.after.tool_results <= 150 &&
            relaxed_tool_floor_result.request.messages.size() >= 3 &&
            relaxed_tool_floor_result.request.messages[
              relaxed_tool_floor_result.request.messages.size() - 3]
                .tool_calls.front()
                .id == "retained-tool-5",
    "hard tool-result limits must relax an oversized recent-exchange floor while retaining "
    "the newest atomic exchange");

  llm_request retention_heavy_conversation;
  retention_heavy_conversation.messages = {
    { .role = "user", .content = std::string(60, 'a') },
    { .role = "assistant", .content = std::string(60, 'b') },
    { .role = "user", .content = std::string(60, 'c') },
  };
  const auto relaxed_conversation_floor_result = manager.fit(retention_heavy_conversation,
    {
      .context_window_tokens = 256,
      .reserved_output_tokens = 10,
      .minimum_recent_conversation_messages = 8,
      .limits = { .conversation = 80 },
    });
  require(relaxed_conversation_floor_result &&
            relaxed_conversation_floor_result.report.preservation_floor_relaxed &&
            relaxed_conversation_floor_result.report.after.conversation <= 80 &&
            !relaxed_conversation_floor_result.request.messages.empty() &&
            relaxed_conversation_floor_result.request.messages.back().content.find('c') !=
              std::string::npos,
    "hard conversation limits must relax an oversized message floor and retain the newest "
    "conversation tail");

  const auto impossible_exchange_result = manager.fit(protected_tool_exchange,
    {
      .context_window_tokens = 512,
      .reserved_output_tokens = 10,
      .limits = { .tool_results = 1 },
    });
  require(!impossible_exchange_result &&
            impossible_exchange_result.report.truncated_messages == 0 &&
            impossible_exchange_result.request.messages[3].content == structured_latest &&
            impossible_exchange_result.report.error.find("tool_results tokens") !=
              std::string::npos,
    "a protected recent Tool exchange must fail a contradictory component limit instead of "
    "corrupting its model-facing projection, and must identify the failing component");
}

void test_context_budget_runner_integration() {
  llm::scripted_llm_client client({ {
    .label = "budgeted request",
    .matches =
      [](const llm_request& request) {
        return request.max_output_tokens == 16 && request.messages.size() == 1;
      },
    .response = { .content = "ok" },
  } });
  llm_agent_runner runner(client);
  llm_request request;
  request.messages.push_back({ .role = "user", .content = "hello" });
  request.context_budget = llm_context_budget {
    .context_window_tokens = 128,
    .reserved_output_tokens = 16,
  };
  const auto restored =
    runtime::llm_codec::request_from_json(runtime::llm_codec::request_to_json(request));
  require(restored.context_budget && restored.context_budget->context_window_tokens == 128,
    "durable continuations must preserve the context budget");
  int reports = 0;
  const auto response = runner.complete(std::move(request),
    {
      .callbacks = { .on_context_budget =
                       [&](const auto& report) {
                         ++reports;
                         require(report.fitted, "runner should publish a fitted context report");
                       } },
    });
  require(response && reports == 1,
    "runner must apply and report the context budget before model dispatch");
}

void test_executor_and_scheduler() {
  manual_executor manual;
  bool manual_ran = false;
  auto manual_task = manual.submit([&](std::stop_token) { manual_ran = true; });
  require(!manual_task.done(), "custom executors should create valid incomplete task handles");
  manual.run_one();
  manual_task.wait();
  require(manual_ran && manual_task.done(),
    "public task completion sources must support custom executors");

  runtime::thread_pool_executor pool({ .threads = 1, .queue_capacity = 1 });
  std::promise<void> started;
  std::promise<void> release;
  auto release_future = release.get_future().share();
  auto first = pool.submit([&](std::stop_token) {
    started.set_value();
    release_future.wait();
  });
  started.get_future().wait();
  auto second = pool.submit([](std::stop_token) {});
  bool rejected = false;
  try {
    (void)pool.submit([](std::stop_token) {});
  }
  catch (const std::runtime_error&) {
    rejected = true;
  }
  require(rejected, "bounded executor must apply queue backpressure");
  release.set_value();
  first.wait();
  second.wait();

  std::promise<void> self_handle_ready;
  std::promise<void> inspect_self;
  auto inspect_self_future = inspect_self.get_future().share();
  std::shared_ptr<runtime::scheduled_task> self_handle;
  std::atomic<bool> self_wait_rejected { false };
  auto self_task = pool.submit([&](std::stop_token) {
    self_handle_ready.set_value();
    inspect_self_future.wait();
    try {
      self_handle->wait();
    }
    catch (const std::logic_error&) {
      self_wait_rejected = true;
    }
  });
  self_handle = std::make_shared<runtime::scheduled_task>(self_task);
  self_handle_ready.get_future().wait();
  inspect_self.set_value();
  self_task.wait();
  require(self_wait_rejected, "scheduled tasks must reject self-waits instead of deadlocking");

  auto concurrent_shutdown_pool = std::make_shared<runtime::thread_pool_executor>(
    runtime::thread_pool_options { .threads = 2, .queue_capacity = 2 });
  std::atomic<int> shutdown_callers { 0 };
  const auto shutdown_from_worker = [&](std::stop_token) {
    ++shutdown_callers;
    while (shutdown_callers.load() != 2)
      std::this_thread::yield();
    concurrent_shutdown_pool->shutdown();
  };
  auto shutdown_one = concurrent_shutdown_pool->submit(shutdown_from_worker);
  auto shutdown_two = concurrent_shutdown_pool->submit(shutdown_from_worker);
  shutdown_one.wait();
  shutdown_two.wait();
  concurrent_shutdown_pool->shutdown();
  require(shutdown_callers == 2,
    "concurrent worker-initiated shutdown must not join the executor domain");

  runtime::timer_scheduler scheduler;
  std::stop_source cancellation;
  bool completed = true;
  const auto cancel_started = std::chrono::steady_clock::now();
  std::jthread waiter(
    [&] { completed = scheduler.wait_for(std::chrono::seconds(1), cancellation.get_token()); });
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  cancellation.request_stop();
  waiter.join();
  require(!completed, "scheduler wait must respond to cancellation");
  require(std::chrono::steady_clock::now() - cancel_started < std::chrono::milliseconds(250),
    "scheduler cancellation must wake promptly");
  auto cancelled_task = scheduler.schedule_after(std::chrono::seconds(1), [](std::stop_token) {});
  cancelled_task.request_stop();
  require(cancelled_task.wait_for(std::chrono::milliseconds(100)),
    "cancelled scheduled work must complete without waiting for its due time");

  auto dispatch_pool = std::make_shared<runtime::thread_pool_executor>(
    runtime::thread_pool_options { .threads = 2, .queue_capacity = 4 });
  runtime::timer_scheduler concurrent_scheduler(dispatch_pool);
  std::promise<void> scheduled_block_started;
  std::promise<void> scheduled_release;
  auto scheduled_release_future = scheduled_release.get_future().share();
  auto blocking =
    concurrent_scheduler.schedule_after(std::chrono::milliseconds::zero(), [&](std::stop_token) {
      scheduled_block_started.set_value();
      scheduled_release_future.wait();
    });
  scheduled_block_started.get_future().wait();
  std::promise<void> independent_completed;
  auto independent_future = independent_completed.get_future();
  auto independent = concurrent_scheduler.schedule_after(
    std::chrono::milliseconds::zero(), [&](std::stop_token) { independent_completed.set_value(); });
  require(independent_future.wait_for(std::chrono::milliseconds(200)) == std::future_status::ready,
    "scheduler callbacks must not block the timer thread");
  scheduled_release.set_value();
  blocking.wait();
  independent.wait();

  std::promise<void> shutdown_started;
  std::promise<void> shutdown_finished;
  auto shutdown_task = concurrent_scheduler.schedule_after(
    std::chrono::milliseconds::zero(), [&](std::stop_token stop_token) {
      shutdown_started.set_value();
      while (!stop_token.stop_requested()) {
        std::this_thread::yield();
      }
      shutdown_finished.set_value();
    });
  shutdown_started.get_future().wait();
  concurrent_scheduler.shutdown();
  require(shutdown_finished.get_future().wait_for(std::chrono::milliseconds(0)) ==
              std::future_status::ready &&
            shutdown_task.done(),
    "scheduler shutdown must join callbacks that were already dispatched");

  auto concurrent_shutdown_scheduler = std::make_shared<runtime::timer_scheduler>(dispatch_pool);
  std::promise<void> concurrent_callback_started;
  std::promise<void> enter_callback_shutdown;
  auto enter_callback_shutdown_future = enter_callback_shutdown.get_future().share();
  auto concurrent_scheduler_task =
    concurrent_shutdown_scheduler->schedule_after(std::chrono::milliseconds::zero(),
      [owner = concurrent_shutdown_scheduler,
        &concurrent_callback_started,
        enter_callback_shutdown_future](std::stop_token) {
        concurrent_callback_started.set_value();
        enter_callback_shutdown_future.wait();
        owner->shutdown();
      });
  concurrent_callback_started.get_future().wait();
  std::jthread release_concurrent_shutdown([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    enter_callback_shutdown.set_value();
  });
  concurrent_shutdown_scheduler->shutdown();
  release_concurrent_shutdown.join();
  concurrent_scheduler_task.wait();
  require(concurrent_scheduler_task.done(),
    "external and callback scheduler shutdown must not wait on each other");

  auto self_dispatch_pool = std::make_shared<runtime::thread_pool_executor>(
    runtime::thread_pool_options { .threads = 1, .queue_capacity = 4 });
  auto self_owned_scheduler = std::make_shared<runtime::timer_scheduler>(self_dispatch_pool);
  std::promise<void> scheduler_callback_started;
  std::promise<void> destroy_scheduler;
  auto destroy_scheduler_future = destroy_scheduler.get_future().share();
  std::promise<void> scheduler_self_destroyed;
  auto scheduler_self_task = self_owned_scheduler->schedule_after(std::chrono::milliseconds::zero(),
    [owner = self_owned_scheduler,
      &scheduler_callback_started,
      destroy_scheduler_future,
      &scheduler_self_destroyed](std::stop_token) mutable {
      scheduler_callback_started.set_value();
      destroy_scheduler_future.wait();
      owner.reset();
      scheduler_self_destroyed.set_value();
    });
  scheduler_callback_started.get_future().wait();
  std::atomic<bool> queued_callback_cancelled { false };
  auto queued_scheduler_task =
    self_owned_scheduler->schedule_after(std::chrono::milliseconds::zero(),
      [&](std::stop_token stop_token) { queued_callback_cancelled = stop_token.stop_requested(); });
  const auto queue_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (self_dispatch_pool->queued() == 0 && std::chrono::steady_clock::now() < queue_deadline) {
    std::this_thread::yield();
  }
  require(self_dispatch_pool->queued() != 0,
    "scheduler regression requires a callback queued in its dispatch domain");
  self_owned_scheduler.reset();
  destroy_scheduler.set_value();
  require(scheduler_self_destroyed.get_future().wait_for(std::chrono::milliseconds(500)) ==
            std::future_status::ready,
    "a scheduler must be destructible from its own dispatched callback");
  scheduler_self_task.wait();
  queued_scheduler_task.wait();
  require(queued_callback_cancelled,
    "self-domain scheduler shutdown must cancel queued dispatched callbacks");

  cancellable_client client;
  llm_agent_runner runner(client);
  auto run_pool = std::make_shared<runtime::thread_pool_executor>(
    runtime::thread_pool_options { .threads = 1, .queue_capacity = 2 });
  {
    auto run = runner.run_async("wait", { .run_executor = run_pool });
    client.started.get_future().wait();
  }
  require(
    client.cancellation_observed, "destroying an async run must cancel and join accepted work");

  cancellable_client temporary_client;
  llm_agent_runner temporary_runner(temporary_client);
  {
    auto run = temporary_runner.run_async("wait",
      {
        .run_executor = std::make_shared<runtime::thread_pool_executor>(
          runtime::thread_pool_options { .threads = 1, .queue_capacity = 2 }),
      });
    temporary_client.started.get_future().wait();
  }
  require(temporary_client.cancellation_observed,
    "temporary injected executors must outlive their accepted async run safely");

  gated_client callback_client;
  llm_agent_runner callback_runner(callback_client);
  auto callback_pool = std::make_shared<runtime::thread_pool_executor>(
    runtime::thread_pool_options { .threads = 1, .queue_capacity = 2 });
  std::optional<llm_agent_run> callback_run;
  std::promise<void> callback_destroyed;
  callback_run.emplace(callback_runner.run_async("finish",
    {
      .run_executor = callback_pool,
      .callbacks = { .on_done =
                       [&](const llm_response&) {
                         callback_run.reset();
                         callback_destroyed.set_value();
                       } },
    }));
  callback_client.started.get_future().wait();
  callback_client.release.set_value();
  require(callback_destroyed.get_future().wait_for(std::chrono::milliseconds(500)) ==
            std::future_status::ready,
    "destroying an async run from its callback must not self-deadlock");
  callback_pool->shutdown();

  auto shared_domain = std::make_shared<runtime::thread_pool_executor>(
    runtime::thread_pool_options { .threads = 2, .queue_capacity = 2 });
  bool same_domain_rejected = false;
  try {
    (void)callback_runner.run_async("invalid",
      {
        .run_executor = shared_domain,
        .tool_executor = shared_domain,
      });
  }
  catch (const std::invalid_argument&) {
    same_domain_rejected = true;
  }
  require(
    same_domain_rejected, "run and tool execution must reject the same blocking executor domain");
}

void test_json_schema_validation() {
  tools::json_schema_validator validator;
  const nlohmann::json schema {
    { "type", "object" },
    { "required", { "name", "count" } },
    { "properties",
      {
        { "name", { { "type", "string" }, { "minLength", 2 } } },
        { "count", { { "type", "integer" }, { "minimum", 1 } } },
      } },
    { "additionalProperties", false },
  };
  require(static_cast<bool>(validator.validate({ { "name", "ok" }, { "count", 2 } }, schema)),
    "valid output should satisfy JSON Schema");
  const auto invalid =
    validator.validate({ { "name", "x" }, { "count", 0 }, { "extra", true } }, schema);
  require(!invalid && invalid.issues.size() >= 3,
    "JSON Schema validator should report independent violations");

  const nlohmann::json referenced {
    { "$defs", { { "id", { { "type", "string" }, { "pattern", "^id-" } } } } },
    { "$ref", "#/$defs/id" },
  };
  require(static_cast<bool>(validator.validate("id-42", referenced)),
    "JSON Schema validator should resolve local references");
  require(!validator.validate("bad", referenced),
    "JSON Schema local reference constraints should be enforced");
  require(!validator.validate(nlohmann::json::object(),
            { { "type", "object" }, { "patternProperties", nlohmann::json::object() } }),
    "unsupported schema assertions must fail closed instead of being ignored");
  require(!validator.validate((std::numeric_limits<double>::quiet_NaN)(), { { "type", "number" } }),
    "JSON Schema validation must reject non-finite instance numbers");
  require(!validator.validate(1.0,
            { { "type", "number" }, { "minimum", (std::numeric_limits<double>::infinity)() } }),
    "JSON Schema validation must reject non-finite numeric assertions");

  tools::tool_descriptor invalid_descriptor {
    .name = "invalid",
    .resource_version = {
      .argument_json_pointer = "/bad~pointer",
    },
  };
  bool invalid_pointer_rejected = false;
  try {
    tools::validate_tool_descriptor(invalid_descriptor);
  }
  catch (const std::invalid_argument&) {
    invalid_pointer_rejected = true;
  }
  require(invalid_pointer_rejected,
    "tool contracts must reject malformed resource-version JSON Pointers");
}

struct test_tool_provider {
  enum class mode {
    retry,
    compensate,
    invalid_output,
    invalid_output_warn,
    missing_version,
    heartbeat,
    heartbeat_timeout,
  };

  mode behavior { mode::retry };
  std::atomic<int> invocations { 0 };
  std::atomic<int> compensations { 0 };

  tools::tool_provider_capabilities contract_capabilities(const std::string&) const noexcept {
    return {
      .invocation_context = true,
      .idempotency_key = true,
      .heartbeat = true,
      .compensation = true,
    };
  }

  std::vector<llm_tool> tools() const {
    return { descriptors().front().model_tool() };
  }

  std::vector<tools::tool_descriptor> descriptors() const {
    tools::tool_descriptor descriptor {
      .name = "contract_tool",
      .description = "exercise the production tool contract",
      .input_schema = {
        { "type", "object" },
        { "properties", {
          { "resource_version", { { "type", "string" } } },
        } },
      },
      .output_schema = {
        { "type", "object" },
        { "required", { "value" } },
        { "properties", {
          { "value", { { "type", "string" } } },
          { "resource_version", { { "type", "string" } } },
        } },
      },
      .idempotency = tools::tool_idempotency::idempotent_with_key,
    };
    descriptor.retry.max_attempts = 2;
    descriptor.retry.initial_backoff = std::chrono::milliseconds::zero();
    descriptor.retry.maximum_backoff = std::chrono::milliseconds::zero();
    descriptor.retry.jitter_ratio = 0.0;
    if (behavior == mode::compensate) {
      descriptor.idempotency = tools::tool_idempotency::non_idempotent;
      descriptor.compensation.enabled = true;
    }
    if (behavior == mode::invalid_output_warn) {
      descriptor.output_validation = tools::tool_output_validation_mode::warn;
    }
    if (behavior == mode::missing_version) {
      descriptor.resource_version.require_expected_version = true;
    }
    if (behavior == mode::heartbeat || behavior == mode::heartbeat_timeout) {
      descriptor.retry.max_attempts = 1;
      descriptor.heartbeat.timeout =
        behavior == mode::heartbeat ? std::chrono::milliseconds(25) : std::chrono::milliseconds(15);
      descriptor.heartbeat.minimum_interval = std::chrono::milliseconds(1);
    }
    return { descriptor };
  }

  llm_tool_result invoke(const tools::tool_invocation& invocation) {
    const auto attempt = ++invocations;
    if (behavior == mode::heartbeat_timeout) {
      std::this_thread::sleep_for(std::chrono::milliseconds(150));
      return { .content = "late", .data = { { "value", "late" } } };
    }
    if (behavior == mode::heartbeat) {
      for (int index = 0; index < 5; ++index) {
        if (invocation.report_heartbeat) {
          invocation.report_heartbeat({
            .message = "working",
            .progress = static_cast<double>(index + 1) / 5.0,
          });
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      return { .content = "done", .data = { { "value", "ok" } } };
    }
    if (behavior == mode::invalid_output || behavior == mode::invalid_output_warn) {
      return { .content = "bad", .data = { { "value", 42 } } };
    }
    if (attempt == 1) {
      if (behavior == mode::compensate) {
        return {
          .content = "partial failure",
          .error_code = std::make_error_code(std::errc::io_error),
          .error_category = tools::tool_error_category::unavailable,
          .retryable = true,
          .compensation_required = true,
          .compensation_token = "undo-1",
        };
      }
      return {
        .content = "transient failure",
        .error_code = std::make_error_code(std::errc::resource_unavailable_try_again),
        .error_category = tools::tool_error_category::unavailable,
        .retryable = true,
      };
    }
    return {
      .content = "ok",
      .data = { { "value", "ok" }, { "resource_version", "v2" } },
    };
  }

  llm_tool_result compensate(const tools::tool_invocation&, const llm_tool_result& failure) {
    ++compensations;
    require(failure.compensation_token == "undo-1",
      "compensation must receive the original failure token");
    return { .content = "rolled back", .data = nlohmann::json::object() };
  }
};

struct legacy_key_provider {
  std::atomic<int> invocations { 0 };

  std::vector<tools::tool_descriptor> descriptors() const {
    tools::tool_descriptor descriptor {
      .name = "contract_tool",
      .description = "legacy provider must not be treated as key-aware",
      .input_schema = { { "type", "object" } },
      .output_schema = { { "type", "object" } },
      .idempotency = tools::tool_idempotency::idempotent_with_key,
    };
    descriptor.retry.max_attempts = 2;
    descriptor.retry.initial_backoff = std::chrono::milliseconds::zero();
    descriptor.retry.maximum_backoff = std::chrono::milliseconds::zero();
    return { descriptor };
  }

  std::vector<llm_tool> tools() const {
    return { descriptors().front().model_tool() };
  }

  llm_tool_result invoke(const std::string&, const std::string&) {
    ++invocations;
    return {
      .content = "unsafe retry",
      .error_code = std::make_error_code(std::errc::resource_unavailable_try_again),
      .error_category = tools::tool_error_category::unavailable,
      .retryable = true,
    };
  }
};

llm::scripted_llm_client scripted_tool_loop(std::string arguments = "{}") {
  return llm::scripted_llm_client({
    {
      .label = "tool call",
      .response = {
        .content = "calling",
        .tool_calls = { {
          .id = "call-1",
          .name = "contract_tool",
          .arguments_json = std::move(arguments),
        } },
      },
    },
    { .label = "final", .response = { .content = "finished" } },
  });
}

void test_tool_contract_runtime() {
  {
    auto provider = std::make_shared<test_tool_provider>();
    auto client = scripted_tool_loop();
    llm_agent_runner runner(client, provider);
    llm_tool_result observed;
    auto response = runner.complete("run",
      {
        .max_in_flight_tool_invocations = 1,
        .callbacks = { .on_tool_result = [&](const auto&,
                                           const auto& result) { observed = result; } },
      });
    require(response && provider->invocations == 2,
      "idempotent retry policy should retry a transient tool failure");
    require(observed.succeeded() && observed.resource_version == "v2",
      "validated tool outcome should expose its resource version");
  }
  {
    auto provider = std::make_shared<test_tool_provider>();
    provider->behavior = test_tool_provider::mode::compensate;
    auto client = scripted_tool_loop();
    llm_agent_runner runner(client, provider);
    auto response = runner.complete("run");
    require(response && provider->invocations == 2 && provider->compensations == 1,
      "non-idempotent retry must compensate a partial failure first");
  }
  {
    auto provider = std::make_shared<test_tool_provider>();
    provider->behavior = test_tool_provider::mode::compensate;
    auto composite = std::make_shared<composite_tool_provider>(provider);
    auto client = scripted_tool_loop();
    llm_agent_runner runner(client, composite);
    auto response = runner.complete("run");
    require(response && provider->invocations == 2 && provider->compensations == 1,
      "composite providers must preserve compensation capability and dispatch");
  }
  {
    auto legacy = std::make_shared<legacy_key_provider>();
    auto composite = std::make_shared<composite_tool_provider>(legacy);
    auto client = scripted_tool_loop();
    llm_agent_runner runner(client, composite);
    llm_tool_result observed;
    auto response = runner.complete("run",
      {
        .callbacks = { .on_tool_result = [&](const auto&,
                                           const auto& result) { observed = result; } },
      });
    require(response && legacy->invocations == 0 &&
              observed.metadata.contains("tool_contract_configuration_error"),
      "legacy providers must not acquire idempotency-key capability through composition");
  }
  {
    auto provider = std::make_shared<test_tool_provider>();
    provider->behavior = test_tool_provider::mode::invalid_output;
    auto client = scripted_tool_loop();
    llm_agent_runner runner(client, provider);
    llm_tool_result observed;
    auto response = runner.complete("run",
      {
        .callbacks = { .on_tool_result = [&](const auto&,
                                           const auto& result) { observed = result; } },
      });
    require(response && !observed.succeeded() && observed.metadata.contains("output_schema_issues"),
      "strict output validation must reject invalid tool data");
  }
  {
    auto provider = std::make_shared<test_tool_provider>();
    provider->behavior = test_tool_provider::mode::invalid_output_warn;
    auto client = scripted_tool_loop();
    llm_agent_runner runner(client, provider);
    llm_tool_result observed;
    auto response = runner.complete("run",
      {
        .callbacks = { .on_tool_result = [&](const auto&,
                                           const auto& result) { observed = result; } },
      });
    require(response && observed.succeeded() && observed.metadata.contains("output_schema_issues"),
      "warning output validation must preserve data and report schema issues");
  }
  {
    auto provider = std::make_shared<test_tool_provider>();
    provider->behavior = test_tool_provider::mode::missing_version;
    auto client = scripted_tool_loop();
    llm_agent_runner runner(client, provider);
    llm_tool_result observed;
    auto response = runner.complete("run",
      {
        .callbacks = { .on_tool_result = [&](const auto&,
                                           const auto& result) { observed = result; } },
      });
    require(response && provider->invocations == 0 &&
              observed.error_category == tools::tool_error_category::invalid_input,
      "resource-version precondition must be enforced before invocation");
  }
  {
    auto provider = std::make_shared<test_tool_provider>();
    provider->behavior = test_tool_provider::mode::heartbeat;
    auto client = scripted_tool_loop();
    llm_agent_runner runner(client, provider);
    std::atomic<int> heartbeats { 0 };
    auto response = runner.complete("run",
      {
        .callbacks = { .on_tool_heartbeat = [&](const auto&, const auto&) { ++heartbeats; } },
      });
    require(
      response && heartbeats > 0, "long-running tools should emit monitored heartbeat events");
  }
  {
    auto provider = std::make_shared<test_tool_provider>();
    provider->behavior = test_tool_provider::mode::heartbeat_timeout;
    auto client = scripted_tool_loop();
    llm_agent_runner runner(client, provider);
    llm_tool_result observed;
    auto response = runner.complete("run",
      {
        .callbacks = { .on_tool_result = [&](const auto&,
                                           const auto& result) { observed = result; } },
      });
    require(response && observed.error_category == tools::tool_error_category::timeout &&
              observed.metadata.contains("heartbeat_timeout"),
      "missing tool heartbeats must isolate the late result");
  }
  {
    auto provider = std::make_shared<test_tool_provider>();
    provider->behavior = test_tool_provider::mode::heartbeat_timeout;
    auto client = scripted_tool_loop();
    llm_agent_runner runner(client, provider);
    const auto started = std::chrono::steady_clock::now();
    auto response = runner.complete("run",
      {
        .tool_executor = std::make_shared<runtime::thread_pool_executor>(
          runtime::thread_pool_options { .threads = 1, .queue_capacity = 2 }),
      });
    const auto elapsed = std::chrono::steady_clock::now() - started;
    require(response && elapsed < std::chrono::milliseconds(100),
      "temporary tool executors must not turn isolated timeouts into destructor waits");
    std::this_thread::sleep_for(std::chrono::milliseconds(170));
  }
}

} // namespace

int main() {
  try {
    test_context_budget();
    test_context_budget_runner_integration();
    test_executor_and_scheduler();
    test_json_schema_validation();
    test_tool_contract_runtime();
    return 0;
  }
  catch (const std::exception& error) {
    std::cerr << "runtime_extensions_tests: " << error.what() << '\n';
    return 1;
  }
}
