#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <wuwe/agent/learning/learning.hpp>
#include <wuwe/agent/training/training.hpp>
#include <wuwe/net/http_client.h>
#include <wuwe/net/http_status_code.h>

namespace {

namespace learning = wuwe::agent::learning;
namespace training = wuwe::agent::training;

void require(bool condition, const std::string& message) {
  if (!condition)
    throw std::runtime_error(message);
}

template<typename Callback>
void require_invalid(Callback&& callback, const std::string& message) {
  try {
    callback();
  }
  catch (const std::invalid_argument&) {
    return;
  }
  throw std::runtime_error(message);
}

template<typename T>
T require_success(training::training_result<T> result, const std::string& message) {
  if (const auto* value = result.value_if())
    return *value;
  const auto* error = result.error_if();
  throw std::runtime_error(message + (error ? ": " + error->message : std::string {}));
}

template<typename T>
void require_error(
  training::training_result<T> result, training::training_errc code, const std::string& message) {
  const auto* error = result.error_if();
  require(error && error->code == code, message);
}

std::string digest(char value) {
  return std::string(64, value);
}

training::training_request request_for(const training::training_dataset_manifest& dataset) {
  return {
    .id = "train-request-1",
    .idempotency_key = "support-agent-qwen-lora-v2",
    .target = "support.model",
    .objective = training::training_objective::supervised_fine_tuning,
    .adaptation = training::lora_training_config {
      .lora = {
        .rank = 16,
        .alpha = 32.0,
        .dropout = 0.05,
        .target_modules = { "q_proj", "v_proj" },
      },
    },
    .base_model = {
      .id = "Qwen/Qwen3-8B",
      .revision = "2f41c0d",
      .digest = digest('a'),
      .tokenizer = "Qwen/Qwen3-8B@2f41c0d",
      .chat_template = "qwen3",
    },
    .dataset = dataset,
    .hyperparameters = {
      .epochs = 2,
      .batch_size = 2,
      .gradient_accumulation_steps = 8,
      .maximum_sequence_length = 4096,
      .learning_rate = 1e-4,
      .seed = 7,
    },
    .resources = {
      .accelerators = 1,
      .accelerator_type = "cuda",
      .memory_gib = 24,
    },
    .output_uri = "s3://models/support/qwen-lora-v2",
  };
}

training::model_artifact_manifest artifact_for(
  const training::training_request& request, std::string provider_job_id = "remote-job-1") {
  return {
    .id = "support-qwen-adapter",
    .version = "adapter-v2",
    .objective = request.objective,
    .adaptation = request.adaptation,
    .uri = request.output_uri + "/adapter",
    .digest = digest('c'),
    .format = "peft.safetensors",
    .base_model = request.base_model,
    .dataset_id = request.dataset.id,
    .dataset_version = request.dataset.version,
    .dataset_digest = request.dataset.digest,
    .tokenizer = request.base_model.tokenizer,
    .chat_template = request.base_model.chat_template,
    .hyperparameters = request.hyperparameters,
    .provider = "test-provider",
    .provider_job_id = std::move(provider_job_id),
  };
}

training::training_job persisted_job(training::training_request request,
  std::string id = "persisted-job", std::string provider = "test-provider",
  std::string tenant_id = {}, std::string workspace_id = {}) {
  const auto now = std::chrono::system_clock::now();
  return {
    .id = std::move(id),
    .provider = std::move(provider),
    .tenant_id = std::move(tenant_id),
    .workspace_id = std::move(workspace_id),
    .revision = 1,
    .request = std::move(request),
    .provider_state = {
      .provider_job_id = "persisted-provider-job",
      .status = training::training_job_status::queued,
    },
    .created_at = now,
    .updated_at = now,
  };
}

class recording_http_client final : public wuwe::http_client {
public:
  wuwe::http_response send(const wuwe::http_request& request) override {
    requests.push_back(request);
    if (responses.empty())
      throw std::runtime_error("missing HTTP response fixture");
    auto response = responses.front();
    responses.erase(responses.begin());
    return response;
  }

  std::vector<wuwe::http_request> requests;
  std::vector<wuwe::http_response> responses;
};

class cancelling_http_client final : public wuwe::http_client {
public:
  explicit cancelling_http_client(std::stop_source& stop_source, wuwe::http_response response)
      : stop_source_(stop_source), response_(std::move(response)) {
  }

  wuwe::http_response send(const wuwe::http_request&) override {
    stop_source_.request_stop();
    return response_;
  }

private:
  std::stop_source& stop_source_;
  wuwe::http_response response_;
};

class cooperatively_cancelling_http_client final : public wuwe::http_client {
public:
  wuwe::http_response send(const wuwe::http_request&) override {
    ++plain_sends;
    return {};
  }

  wuwe::http_response send_stream(const wuwe::http_request&,
    const wuwe::http_stream_chunk_callback&, std::stop_token stop_token) override {
    ++stream_sends;
    observed_token = stop_token.stop_possible();
    if (stop_source)
      stop_source->request_stop();
    return { .error_code = std::make_error_code(std::errc::operation_canceled),
      .transport_error = std::make_error_code(std::errc::operation_canceled) };
  }

  std::stop_source* stop_source {};
  std::size_t plain_sends {};
  std::size_t stream_sends {};
  bool observed_token {};
};

class failing_training_job_store final : public training::training_job_store {
public:
  bool fail_create {};
  bool fail_reads {};

  [[nodiscard]] wuwe::agent::core::storage_capabilities capabilities() const noexcept override {
    return {
      .declared = true,
      .optimistic_concurrency = true,
      .atomic_mutations = true,
      .coordination_scope = wuwe::agent::core::storage_coordination_scope::process_local,
      .schema_version = 1,
    };
  }

  training::training_result<training::training_job_create_result> create(
    training::training_job value) override {
    if (fail_create)
      return training::training_result<training::training_job_create_result>::failure({
        .code = training::training_errc::persistence_failure,
        .message = "injected create failure",
        .retryable = true,
      });
    value.revision = 1;
    job = value;
    return training::training_result<training::training_job_create_result>::success(
      { .job = std::move(value), .inserted = true });
  }

  [[nodiscard]] training::training_result<std::optional<training::training_job>> load(
    const std::string&) const override {
    if (fail_reads)
      return training::training_result<std::optional<training::training_job>>::failure({
        .code = training::training_errc::corrupted_state,
        .message = "injected corrupted state",
      });
    return training::training_result<std::optional<training::training_job>>::success(job);
  }

  [[nodiscard]] training::training_result<std::optional<training::training_job>>
  find_by_idempotency_key(
    const std::string&, const std::string&, const std::string&) const override {
    if (fail_reads)
      return training::training_result<std::optional<training::training_job>>::failure({
        .code = training::training_errc::persistence_failure,
        .message = "injected read failure",
        .retryable = true,
      });
    return training::training_result<std::optional<training::training_job>>::success(job);
  }

  [[nodiscard]] training::training_result<std::vector<training::training_job>>
  list() const override {
    if (fail_reads)
      return training::training_result<std::vector<training::training_job>>::failure({
        .code = training::training_errc::corrupted_state,
        .message = "injected corrupted state",
      });
    return training::training_result<std::vector<training::training_job>>::success(
      job ? std::vector { *job } : std::vector<training::training_job> {});
  }

  training::training_result<training::training_job> update(
    training::training_job value, std::uint64_t) override {
    job = value;
    return training::training_result<training::training_job>::success(std::move(value));
  }

private:
  std::optional<training::training_job> job;
};

class scripted_training_job_store final : public training::training_job_store {
public:
  using create_callback =
    std::function<training::training_result<training::training_job_create_result>(
      training::training_job)>;
  using load_callback =
    std::function<training::training_result<std::optional<training::training_job>>(
      const std::string&)>;
  using find_callback =
    std::function<training::training_result<std::optional<training::training_job>>(
      const std::string&, const std::string&, const std::string&)>;
  using update_callback = std::function<training::training_result<training::training_job>(
    training::training_job, std::uint64_t)>;

  [[nodiscard]] wuwe::agent::core::storage_capabilities capabilities() const noexcept override {
    return {
      .declared = true,
      .optimistic_concurrency = true,
      .atomic_mutations = true,
      .coordination_scope = wuwe::agent::core::storage_coordination_scope::process_local,
      .schema_version = 1,
    };
  }

  training::training_result<training::training_job_create_result> create(
    training::training_job value) override {
    if (create_fn)
      return create_fn(std::move(value));
    value.revision = 1;
    value.updated_at = std::chrono::system_clock::now();
    job = value;
    return training::training_result<training::training_job_create_result>::success(
      { .job = std::move(value), .inserted = true });
  }

  [[nodiscard]] training::training_result<std::optional<training::training_job>> load(
    const std::string& id) const override {
    if (load_fn)
      return load_fn(id);
    return training::training_result<std::optional<training::training_job>>::success(
      job && job->id == id ? job : std::nullopt);
  }

  [[nodiscard]] training::training_result<std::optional<training::training_job>>
  find_by_idempotency_key(const std::string& tenant_id, const std::string& workspace_id,
    const std::string& key) const override {
    if (find_fn)
      return find_fn(tenant_id, workspace_id, key);
    return training::training_result<std::optional<training::training_job>>::success(
      job && job->tenant_id == tenant_id && job->workspace_id == workspace_id &&
          job->request.idempotency_key == key
        ? job
        : std::nullopt);
  }

  [[nodiscard]] training::training_result<std::vector<training::training_job>>
  list() const override {
    return training::training_result<std::vector<training::training_job>>::success(
      job ? std::vector { *job } : std::vector<training::training_job> {});
  }

  training::training_result<training::training_job> update(
    training::training_job value, std::uint64_t expected_revision) override {
    if (update_fn)
      return update_fn(std::move(value), expected_revision);
    value.revision = expected_revision + 1;
    if (job)
      value.created_at = job->created_at;
    value.updated_at = std::chrono::system_clock::now();
    job = value;
    return training::training_result<training::training_job>::success(std::move(value));
  }

  create_callback create_fn;
  load_callback load_fn;
  find_callback find_fn;
  update_callback update_fn;
  std::optional<training::training_job> job;
};

class changing_name_training_provider final : public training::training_provider {
public:
  explicit changing_name_training_provider(std::string first_name)
      : first_name_(std::move(first_name)) {
  }

  [[nodiscard]] std::string name() const override {
    ++name_calls;
    return name_calls == 1 ? first_name_ : "changed-provider";
  }

  [[nodiscard]] training::training_result<training::training_provider_snapshot> submit(
    const training::training_request&, const training::training_context&) override {
    return training::training_result<training::training_provider_snapshot>::success({
      .provider_job_id = "frozen-provider-job",
      .status = training::training_job_status::queued,
    });
  }

  [[nodiscard]] training::training_result<training::training_provider_snapshot> inspect(
    const std::string& id, const training::training_context&) override {
    return training::training_result<training::training_provider_snapshot>::success({
      .provider_job_id = id,
      .status = training::training_job_status::running,
    });
  }

  [[nodiscard]] training::training_result<training::training_provider_snapshot> cancel(
    const std::string& id, const training::training_context&) override {
    return training::training_result<training::training_provider_snapshot>::success({
      .provider_job_id = id,
      .status = training::training_job_status::cancelled,
    });
  }

  mutable std::size_t name_calls {};

private:
  std::string first_name_;
};

class recording_approval_service final : public wuwe::agent::approval::approval_service {
public:
  explicit recording_approval_service(wuwe::agent::approval::approval_decision_kind kind)
      : kind_(kind) {
  }

  [[nodiscard]] wuwe::agent::approval::approval_decision decide(
    const wuwe::agent::approval::approval_request& value) override {
    request = value;
    return { .kind = kind_, .reason = wuwe::agent::approval::to_string(kind_) };
  }

  std::optional<wuwe::agent::approval::approval_request> request;

private:
  wuwe::agent::approval::approval_decision_kind kind_;
};

class throwing_approval_service final : public wuwe::agent::approval::approval_service {
public:
  [[nodiscard]] wuwe::agent::approval::approval_decision decide(
    const wuwe::agent::approval::approval_request&) override {
    throw std::runtime_error("approval backend unavailable");
  }
};

bool has_header(
  const wuwe::http_request& request, const std::string& name, const std::string& value) {
  for (const auto& [candidate_name, candidate_value] : request.headers) {
    if (candidate_name == name && candidate_value == value)
      return true;
  }
  return false;
}

training::training_dataset sample_dataset() {
  std::vector<learning::experience_record> experiences {
    {
      .id = "experience-1",
      .source = "production-feedback",
      .source_run_id = "run-1",
      .input = "How do I reset my token?",
      .output = "An incomplete answer.",
      .expected_output = "Open Settings, revoke the old token, and create a new one.",
      .feedback_type = learning::feedback_kind::correction,
    },
    {
      .id = "experience-2",
      .input = "No reviewed response",
      .output = "Unreviewed output",
    },
  };
  return learning::build_sft_dataset(experiences,
    {
      .dataset_id = "support-sft",
      .version = "2026-08-13",
      .uri = "s3://datasets/support-sft/2026-08-13.jsonl",
      .system_prompt = "You are a grounded support agent.",
      .tokenizer = "Qwen/Qwen3-8B@2f41c0d",
      .chat_template = "qwen3",
    });
}

void builds_reproducible_sft_datasets() {
  const auto dataset = sample_dataset();
  require(dataset.examples.size() == 1 && dataset.manifest.example_count == 1 &&
            dataset.manifest.message_count == 3,
    "SFT builder includes reviewed examples and records exact counts");
  require(training::canonical_sha256(dataset.manifest.digest),
    "SFT builder computes a canonical content digest");
  require(dataset.examples.front().messages.back().trainable &&
            dataset.examples.front().messages.back().content.find("revoke") != std::string::npos,
    "SFT builder trains the reviewed assistant response");
  const auto jsonl = training::export_training_jsonl(dataset);
  require(jsonl.find("wuwe") == std::string::npos &&
            jsonl.find("expected_output") == std::string::npos && jsonl.back() == '\n',
    "dataset export emits provider-neutral message JSONL without ledger internals");

  require_invalid(
    [&] {
      learning::build_sft_dataset({},
        {
          .dataset_id = "empty",
          .version = "v1",
        });
    },
    "empty training datasets must be rejected");

  auto mutated = dataset;
  mutated.examples.front().messages.back().content = "tampered after digest creation";
  require_invalid([&] { training::validate_training_dataset(mutated); },
    "dataset validation must reject sample mutation with a stale digest");

  const std::vector<learning::experience_record> anonymous_experiences {
    {
      .input = "How do I rotate a key?",
      .expected_output = "Create a replacement, deploy it, then revoke the old key.",
      .feedback_type = learning::feedback_kind::correction,
    },
  };
  const learning::sft_dataset_builder_options anonymous_options {
    .dataset_id = "anonymous-sft",
    .version = "v1",
  };
  const auto first = learning::build_sft_dataset(anonymous_experiences, anonymous_options);
  const auto second = learning::build_sft_dataset(anonymous_experiences, anonymous_options);
  require(first.examples.front().id == second.examples.front().id &&
            first.manifest.digest == second.manifest.digest &&
            first.examples.front().id.starts_with("training-example-"),
    "records without source IDs receive deterministic content-derived example IDs");

  const auto mapper = [](const learning::experience_record& experience) {
    return training::training_example {
      .messages = {
        { .role = training::training_message_role::user, .content = experience.input },
        { .role = training::training_message_role::assistant,
          .content = experience.expected_output, .trainable = true },
      },
    };
  };
  const auto mapped_first =
    learning::build_sft_dataset(anonymous_experiences, anonymous_options, mapper);
  const auto mapped_second =
    learning::build_sft_dataset(anonymous_experiences, anonymous_options, mapper);
  require(mapped_first.examples.front().id == mapped_second.examples.front().id &&
            mapped_first.manifest.digest == mapped_second.manifest.digest,
    "custom mappers that omit IDs receive the same deterministic identity guarantee");
}

void validates_agent_tool_trajectories() {
  training::training_example example {
    .id = "tool-example",
    .tools = {
      {
        .name = "get_order",
        .description = "Load an order by ID",
        .parameters = {
          { "type", "object" },
          { "properties", { { "id", { { "type", "integer" } } } } },
          { "required", { "id" } },
        },
      },
    },
    .messages = {
      { .role = training::training_message_role::user, .content = "Find order 42" },
      {
        .role = training::training_message_role::assistant,
        .tool_calls = {
          { .id = "call-1", .name = "get_order", .arguments = { { "id", 42 } } },
        },
        .trainable = true,
      },
      {
        .role = training::training_message_role::tool,
        .content = R"({"status":"shipped"})",
        .tool_call_id = "call-1",
      },
      {
        .role = training::training_message_role::assistant,
        .content = "Order 42 has shipped.",
        .trainable = true,
      },
    },
  };
  training::validate_training_example(example);

  auto unmatched = example;
  unmatched.messages[2].tool_call_id = "unknown";
  require_invalid([&] { training::validate_training_example(unmatched); },
    "unmatched tool results must be rejected");

  auto interrupted = example;
  interrupted.messages.insert(interrupted.messages.begin() + 2,
    { .role = training::training_message_role::assistant, .content = "interrupted" });
  require_invalid([&] { training::validate_training_example(interrupted); },
    "tool results must stay adjacent to their calls");

  auto undeclared = example;
  undeclared.tools.clear();
  require_invalid([&] { training::validate_training_example(undeclared); },
    "tool calls without the model-visible tool schema must be rejected");

  auto reused = example;
  reused.messages.push_back({
    .role = training::training_message_role::assistant,
    .tool_calls = {
      { .id = "call-1", .name = "get_order", .arguments = { { "id", 43 } } },
    },
    .trainable = true,
  });
  reused.messages.push_back({
    .role = training::training_message_role::tool,
    .content = R"({"status":"processing"})",
    .tool_call_id = "call-1",
  });
  require_invalid([&] { training::validate_training_example(reused); },
    "tool call IDs cannot be reused after an earlier call has completed");
}

void validates_qlora_as_a_real_adaptation_contract() {
  auto qlora = request_for(sample_dataset().manifest);
  qlora.adaptation = training::qlora_training_config {
    .lora = *training::lora_config_if(qlora.adaptation),
    .quantization = {
      .bits = 4,
      .quantization_type = "nf4",
      .compute_dtype = "bfloat16",
      .double_quantization = true,
    },
  };
  training::validate_training_request(qlora);
  require(training::adaptation_kind(qlora.adaptation) == training::training_adaptation::qlora &&
            training::quantization_config_if(qlora.adaptation) != nullptr,
    "QLoRA is represented by one complete, unambiguous variant");
}

void validates_training_budget_and_resources() {
  const auto dataset = sample_dataset();
  auto request = request_for(dataset.manifest);
  training::validate_training_request(request);

  auto step_budget = request;
  step_budget.hyperparameters.epochs = 0;
  step_budget.hyperparameters.maximum_steps = 500;
  training::validate_training_request(step_budget);

  const auto reject = [&](auto mutate, const std::string& message) {
    auto invalid = request;
    mutate(invalid);
    require_invalid([&] { training::validate_training_request(invalid); }, message);
  };
  reject([](auto& value) { value.hyperparameters.maximum_steps = 500; },
    "training requests must choose epochs or maximum steps, not both");
  reject([](auto& value) { value.hyperparameters.epochs = 0; },
    "training requests require one explicit training budget");
  reject(
    [](auto& value) {
      value.hyperparameters.numeric["loss_scale"] = std::numeric_limits<double>::infinity();
    },
    "numeric training hyperparameters must be finite");
  reject([](auto& value) { value.resources.accelerators = 0; },
    "training requests require at least one accelerator");
  reject([](auto& value) { value.resources.accelerator_type.clear(); },
    "training requests require an accelerator type");
  reject([](auto& value) { value.resources.memory_gib = 0; },
    "training requests require an accelerator memory budget");
  reject([](auto& value) { value.resources.maximum_runtime = std::chrono::seconds(-1); },
    "training requests reject negative maximum runtime");
}

void compares_provider_history_at_protocol_time_precision() {
  const auto base_time = std::chrono::system_clock::time_point(
    std::chrono::milliseconds(1000) + std::chrono::microseconds(123));
  const auto same_millisecond = base_time + std::chrono::microseconds(500);
  const auto next_millisecond = base_time + std::chrono::milliseconds(1);
  const training::training_metric metric {
    .step = 1, .name = "loss", .value = 0.5, .observed_at = base_time
  };
  auto equivalent_metric = metric;
  equivalent_metric.observed_at = same_millisecond;
  auto changed_metric = metric;
  changed_metric.observed_at = next_millisecond;
  require(training::same_training_metric(metric, equivalent_metric) &&
            !training::same_training_metric(metric, changed_metric),
    "metric timestamp identity follows persisted millisecond precision");

  const training::training_checkpoint checkpoint { .id = "checkpoint",
    .uri = "s3://checkpoint",
    .digest = digest('a'),
    .step = 1,
    .created_at = base_time };
  auto equivalent_checkpoint = checkpoint;
  equivalent_checkpoint.created_at = same_millisecond;
  auto changed_checkpoint = checkpoint;
  changed_checkpoint.created_at = next_millisecond;
  require(training::same_training_checkpoint(checkpoint, equivalent_checkpoint) &&
            !training::same_training_checkpoint(checkpoint, changed_checkpoint),
    "checkpoint timestamp identity follows persisted millisecond precision");
}

void rejects_contradictory_adaptation_json() {
  auto encoded = training::training_request_to_json(request_for(sample_dataset().manifest));
  encoded["adaptation"] = "full";
  require_invalid([&] { (void)training::training_request_from_json(encoded); },
    "full training JSON must reject LoRA fields");

  encoded = training::training_request_to_json(request_for(sample_dataset().manifest));
  encoded["adaptation"] = "full";
  encoded["lora"] = nullptr;
  encoded["quantization"] = {
    { "bits", 4 },
    { "quantization_type", "nf4" },
    { "compute_dtype", "bfloat16" },
    { "double_quantization", true },
  };
  require_invalid([&] { (void)training::training_request_from_json(encoded); },
    "full training JSON must reject quantization fields");

  encoded = training::training_request_to_json(request_for(sample_dataset().manifest));
  encoded["quantization"] = {
    { "bits", 4 },
    { "quantization_type", "nf4" },
    { "compute_dtype", "bfloat16" },
    { "double_quantization", true },
  };
  require_invalid([&] { (void)training::training_request_from_json(encoded); },
    "LoRA JSON must reject quantization fields");

  encoded["adaptation"] = "qlora";
  encoded["lora"] = nullptr;
  require_invalid([&] { (void)training::training_request_from_json(encoded); },
    "QLoRA JSON must require LoRA fields");

  encoded = training::training_request_to_json(request_for(sample_dataset().manifest));
  encoded["adaptation"] = "qlora";
  encoded["quantization"] = nullptr;
  require_invalid([&] { (void)training::training_request_from_json(encoded); },
    "QLoRA JSON must require quantization fields");
}

void classifies_submission_failures_and_reconciliation() {
  const auto request = request_for(sample_dataset().manifest);
  auto provider = std::make_shared<training::function_training_provider>(
    "test-provider",
    [](const training::training_request&, const training::training_context&) {
      return training::training_provider_snapshot {
        .provider_job_id = "remote-reconcile-1",
        .status = training::training_job_status::queued,
      };
    },
    [](const std::string& id, const training::training_context&) {
      return training::training_provider_snapshot { .provider_job_id = id,
        .status = training::training_job_status::running };
    },
    [](const std::string& id, const training::training_context&) {
      return training::training_provider_snapshot { .provider_job_id = id,
        .status = training::training_job_status::cancelled };
    });
  failing_training_job_store store;
  store.fail_create = true;
  training::training_coordinator coordinator(provider,
    store,
    {
      .policy = training::trusted_training_policy(),
    });
  const auto failed = coordinator.submit(request, { .request_id = "request-reconcile-1" });
  const auto* error = failed.error_if();
  require(error && error->code == training::training_errc::reconciliation_required &&
            error->details.at("provider") == "test-provider" &&
            error->details.at("provider_job_id") == "remote-reconcile-1" &&
            error->details.at("request_id") == "request-reconcile-1" &&
            error->details.at("idempotency_key") == request.idempotency_key,
    "remote success followed by local failure requires explicit reconciliation coordinates");

  auto invalid_provider = std::make_shared<training::function_training_provider>(
    "test-provider",
    [](const training::training_request&, const training::training_context&) {
      return training::training_provider_snapshot { .provider_job_id = "",
        .status = training::training_job_status::queued };
    },
    [](const std::string&, const training::training_context&) {
      return training::training_provider_snapshot {};
    },
    [](const std::string&, const training::training_context&) {
      return training::training_provider_snapshot {};
    });
  training::in_memory_training_job_store valid_store;
  training::training_coordinator invalid(invalid_provider,
    valid_store,
    {
      .policy = training::trusted_training_policy(),
    });
  require_error(invalid.submit(request),
    training::training_errc::reconciliation_required,
    "invalid initial success snapshots require reconciliation because remote state may exist");

  failing_training_job_store read_store;
  read_store.fail_reads = true;
  training::training_coordinator unreadable(provider,
    read_store,
    {
      .policy = training::trusted_training_policy(),
    });
  require_error(unreadable.submit(request),
    training::training_errc::persistence_failure,
    "typed Store read failures remain distinguishable from missing jobs");
  require_error(unreadable.load("job"),
    training::training_errc::corrupted_state,
    "typed Store load failures preserve corrupted-state semantics");
  require_error(read_store.list(),
    training::training_errc::corrupted_state,
    "typed Store list failures preserve corrupted-state semantics");
}

void reconciles_ambiguous_remote_mutation_failures() {
  const auto request = request_for(sample_dataset().manifest);
  const auto uncertain = [](std::string message) {
    return training::training_result<training::training_provider_snapshot>::failure({
      .code = training::training_errc::provider_transport,
      .message = std::move(message),
      .retryable = true,
      .remote_outcome = training::training_remote_outcome::uncertain,
    });
  };
  auto submit_provider = std::make_shared<training::function_training_provider>(
    "uncertain-provider",
    [uncertain](const training::training_request&, const training::training_context&) {
      return uncertain("submit response lost");
    },
    [](const std::string& id, const training::training_context&) {
      return training::training_result<training::training_provider_snapshot>::success(
        { .provider_job_id = id, .status = training::training_job_status::running });
    },
    [](const std::string& id, const training::training_context&) {
      return training::training_result<training::training_provider_snapshot>::success(
        { .provider_job_id = id, .status = training::training_job_status::cancelled });
    });
  training::in_memory_training_job_store submit_store;
  training::training_coordinator submit_coordinator(submit_provider,
    submit_store,
    {
      .policy = training::trusted_training_policy(),
    });
  const auto submitted = submit_coordinator.submit(request, { .request_id = "uncertain-submit" });
  require(submitted.error_if() &&
            submitted.error_if()->code == training::training_errc::reconciliation_required &&
            submitted.error_if()->remote_outcome == training::training_remote_outcome::uncertain &&
            submitted.error_if()->details.at("operation") == "training.submit" &&
            submitted.error_if()->details.at("provider_job_id") == "" &&
            submitted.error_if()->details.at("idempotency_key") == request.idempotency_key,
    "ambiguous submit failures expose idempotency-based reconciliation coordinates");

  std::size_t cancellations {};
  std::size_t resumptions {};
  auto lifecycle_provider = std::make_shared<training::function_training_provider>(
    "uncertain-provider",
    [](const training::training_request&, const training::training_context&) {
      return training::training_result<training::training_provider_snapshot>::success({
        .provider_job_id = "uncertain-job",
        .status = training::training_job_status::paused,
        .current_step = 10,
        .latest_checkpoint =
          training::training_checkpoint {
            .id = "checkpoint-10",
            .uri = "s3://checkpoint/10",
            .digest = digest('e'),
            .step = 10,
          },
      });
    },
    [](const std::string& id, const training::training_context&) {
      return training::training_result<training::training_provider_snapshot>::success(
        { .provider_job_id = id,
          .status = training::training_job_status::paused,
          .current_step = 10 });
    },
    [&cancellations, uncertain](const std::string&, const training::training_context&) {
      ++cancellations;
      return uncertain("cancel response lost");
    },
    [&resumptions, uncertain](
      const std::string&, const training::training_checkpoint&, const training::training_context&) {
      ++resumptions;
      return uncertain("resume response lost");
    });
  training::in_memory_training_job_store cancel_store;
  training::training_coordinator cancel_coordinator(lifecycle_provider,
    cancel_store,
    {
      .policy = training::trusted_training_policy(),
    });
  const auto cancel_job = require_success(cancel_coordinator.submit(request), "submit failed");
  const auto cancelled = cancel_coordinator.cancel(cancel_job.id);
  require(cancelled.error_if() &&
            cancelled.error_if()->code == training::training_errc::reconciliation_required &&
            cancelled.error_if()->details.at("operation") == "training.cancel" &&
            cancelled.error_if()->details.at("provider_job_id") == "uncertain-job" &&
            cancelled.error_if()->details.at("local_revision") == 1 && cancellations == 1,
    "ambiguous cancel failures require reconciliation against the known provider job");

  training::in_memory_training_job_store resume_store;
  training::training_coordinator resume_coordinator(lifecycle_provider,
    resume_store,
    {
      .policy = training::trusted_training_policy(),
    });
  const auto resume_job = require_success(resume_coordinator.submit(request), "submit failed");
  const auto resumed = resume_coordinator.resume(resume_job.id);
  require(resumed.error_if() &&
            resumed.error_if()->code == training::training_errc::reconciliation_required &&
            resumed.error_if()->details.at("operation") == "training.resume" &&
            resumed.error_if()->details.at("provider_job_id") == "uncertain-job" &&
            resumed.error_if()->details.at("local_revision") == 1 && resumptions == 1,
    "ambiguous resume failures require reconciliation against the known provider job");
}

void reconciles_uncertain_submissions_end_to_end() {
  const auto request = request_for(sample_dataset().manifest);
  std::size_t lookups {};
  auto provider = std::make_shared<training::function_training_provider>("reconcile-provider",
    training::function_training_provider::submit_callback(
      [](const training::training_request&, const training::training_context&) {
        return training::training_result<training::training_provider_snapshot>::failure({
          .code = training::training_errc::provider_transport,
          .message = "submit response lost",
          .retryable = true,
          .remote_outcome = training::training_remote_outcome::uncertain,
        });
      }),
    training::function_training_provider::inspect_callback(
      [](const std::string& id, const training::training_context&) {
        return training::training_result<training::training_provider_snapshot>::success(
          { .provider_job_id = id, .status = training::training_job_status::running });
      }),
    training::function_training_provider::cancel_callback(
      [](const std::string& id, const training::training_context&) {
        return training::training_result<training::training_provider_snapshot>::success(
          { .provider_job_id = id, .status = training::training_job_status::cancelled });
      }),
    training::function_training_provider::resume_callback {},
    training::function_training_provider::lookup_callback(
      [&lookups,
        key = request.idempotency_key,
        request_id = request.id,
        request_digest = training::training_request_digest(request)](
        const std::string& idempotency_key, const training::training_context&) {
        ++lookups;
        if (idempotency_key != key)
          return training::training_result<
            std::optional<training::training_submission_record>>::success(std::nullopt);
        return training::training_result<std::optional<training::training_submission_record>>::success(
        training::training_submission_record {
          .idempotency_key = key,
          .request_id = request_id,
          .request_digest = request_digest,
          .snapshot = {
            .provider_job_id = "recovered-provider-job",
            .status = training::training_job_status::queued,
          },
        });
      }));
  training::in_memory_training_job_store store;
  training::training_coordinator coordinator(provider,
    store,
    {
      .policy = training::trusted_training_policy(),
    });
  require_error(coordinator.submit(request),
    training::training_errc::reconciliation_required,
    "uncertain submit enters reconciliation");
  const auto recovered =
    require_success(coordinator.reconcile_submit(request), "submission reconciliation failed");
  const auto duplicate = require_success(
    coordinator.reconcile_submit(request), "idempotent submission reconciliation failed");
  require(recovered.provider_state.provider_job_id == "recovered-provider-job" &&
            recovered.revision == 1 && duplicate.id == recovered.id && lookups == 1,
    "reconciliation looks up the Provider submission and persists it exactly once");

  auto unsupported_provider = std::make_shared<training::function_training_provider>(
    "unsupported-reconcile",
    [](const training::training_request&, const training::training_context&) {
      return training::training_provider_snapshot { .provider_job_id = "job",
        .status = training::training_job_status::queued };
    },
    [](const std::string& id, const training::training_context&) {
      return training::training_provider_snapshot { .provider_job_id = id,
        .status = training::training_job_status::queued };
    },
    [](const std::string& id, const training::training_context&) {
      return training::training_provider_snapshot { .provider_job_id = id,
        .status = training::training_job_status::cancelled };
    });
  training::in_memory_training_job_store unsupported_store;
  training::training_coordinator unsupported(unsupported_provider,
    unsupported_store,
    {
      .policy = training::trusted_training_policy(),
    });
  require_error(unsupported.reconcile_submit(request),
    training::training_errc::unsupported_operation,
    "Providers without lookup support report an explicit unsupported operation");
}

void validates_reconciliation_submission_identity() {
  const auto request = request_for(sample_dataset().manifest);
  const auto make_provider = [](training::function_training_provider::lookup_callback lookup) {
    return std::make_shared<training::function_training_provider>("identity-provider",
      training::function_training_provider::submit_callback(
        [](const training::training_request&, const training::training_context&) {
          return training::training_result<training::training_provider_snapshot>::success({
            .provider_job_id = "unused-submit",
            .status = training::training_job_status::queued,
          });
        }),
      training::function_training_provider::inspect_callback(
        [](const std::string& id, const training::training_context&) {
          return training::training_result<training::training_provider_snapshot>::success({
            .provider_job_id = id,
            .status = training::training_job_status::running,
          });
        }),
      training::function_training_provider::cancel_callback(
        [](const std::string& id, const training::training_context&) {
          return training::training_result<training::training_provider_snapshot>::success({
            .provider_job_id = id,
            .status = training::training_job_status::cancelled,
          });
        }),
      training::function_training_provider::resume_callback {},
      std::move(lookup));
  };

  training::in_memory_training_job_store mismatched_store;
  training::training_coordinator mismatched(
    make_provider([&](const std::string&, const training::training_context&) {
      return training::training_result<std::optional<training::training_submission_record>>::success(
        training::training_submission_record {
          .idempotency_key = request.idempotency_key,
          .request_id = request.id,
          .request_digest = digest('f'),
          .snapshot = {
            .provider_job_id = "wrong-request-job",
            .status = training::training_job_status::queued,
          },
        });
    }),
    mismatched_store,
    { .policy = training::trusted_training_policy() });
  require_error(mismatched.reconcile_submit(request),
    training::training_errc::invalid_request,
    "reconciliation rejects a Provider submission bound to different request content");
  require(require_success(mismatched_store.list(), "Store list failed").empty(),
    "identity-mismatched Provider submissions are never persisted");

  training::in_memory_training_job_store malformed_store;
  training::training_coordinator malformed(
    make_provider([&](const std::string&, const training::training_context&) {
      return training::training_result<std::optional<training::training_submission_record>>::success(
        training::training_submission_record {
          .idempotency_key = request.idempotency_key,
          .request_id = request.id,
          .request_digest = training::training_request_digest(request),
          .snapshot = {
            .provider_job_id = "malformed-recovered-job",
            .status = training::training_job_status::succeeded,
          },
        });
    }),
    malformed_store,
    { .policy = training::trusted_training_policy() });
  const auto malformed_result = malformed.reconcile_submit(request);
  const auto* malformed_error = malformed_result.error_if();
  require(malformed_error &&
            malformed_error->code == training::training_errc::reconciliation_required &&
            malformed_error->remote_outcome == training::training_remote_outcome::applied &&
            malformed_error->details.at("provider_job_id") == "malformed-recovered-job" &&
            require_success(malformed_store.list(), "Store list failed").empty(),
    "malformed recovered submissions preserve the applied remote outcome and require repair");
}

void validates_error_boundaries_and_best_effort_telemetry() {
  require_invalid(
    [] {
      (void)training::training_result<int>::failure({
        .code = training::training_errc::provider_protocol,
        .message = "",
      });
    },
    "training_result rejects invalid errors at construction time");

  const auto request = request_for(sample_dataset().manifest);
  std::size_t submissions {};
  auto provider = std::make_shared<training::function_training_provider>("boundary-provider",
    training::function_training_provider::submit_callback(
      [&submissions](const training::training_request&, const training::training_context&) {
        ++submissions;
        return training::training_result<training::training_provider_snapshot>::success({
          .provider_job_id = "boundary-job",
          .status = training::training_job_status::queued,
        });
      }),
    training::function_training_provider::inspect_callback(
      [](const std::string&, const training::training_context&) {
        return training::training_result<training::training_provider_snapshot>::failure({
          .code = training::training_errc::provider_protocol,
          .message = "invalid read-only effect",
          .remote_outcome = training::training_remote_outcome::applied,
        });
      }),
    training::function_training_provider::cancel_callback(
      [](const std::string& id, const training::training_context&) {
        return training::training_result<training::training_provider_snapshot>::success({
          .provider_job_id = id,
          .status = training::training_job_status::cancelled,
        });
      }),
    training::function_training_provider::resume_callback {},
    training::function_training_provider::lookup_callback(
      [](const std::string&, const training::training_context&) {
        return training::training_result<
          std::optional<training::training_submission_record>>::failure({
          .code = training::training_errc::provider_protocol,
          .message = "invalid lookup effect",
          .remote_outcome = training::training_remote_outcome::uncertain,
        });
      }));
  const auto inspect = provider->inspect("job", {});
  const auto lookup = provider->lookup_submission(request.idempotency_key, {});
  require(
    inspect.error_if() && inspect.error_if()->code == training::training_errc::provider_protocol &&
      inspect.error_if()->remote_outcome == training::training_remote_outcome::not_applicable &&
      lookup.error_if() && lookup.error_if()->code == training::training_errc::provider_protocol &&
      lookup.error_if()->remote_outcome == training::training_remote_outcome::not_applicable,
    "read-only Provider boundaries reject mutation outcomes without leaking invalid errors");

  scripted_training_job_store invalid_store;
  invalid_store.find_fn = [](const std::string&, const std::string&, const std::string&) {
    return training::training_result<std::optional<training::training_job>>::failure({
      .code = training::training_errc::persistence_failure,
      .message = "invalid Store remote effect",
      .remote_outcome = training::training_remote_outcome::not_applied,
    });
  };
  training::training_coordinator invalid_store_coordinator(
    provider, invalid_store, { .policy = training::trusted_training_policy() });
  require_error(invalid_store_coordinator.submit(request),
    training::training_errc::corrupted_state,
    "Coordinator converts operation-invalid Store errors into corrupted state");
  require(submissions == 0, "invalid Store errors stop submission before Provider dispatch");

  training::in_memory_training_job_store telemetry_store;
  training::training_coordinator telemetry_coordinator(provider,
    telemetry_store,
    {
      .observer =
        [](const training::training_job&) { throw std::runtime_error("observer unavailable"); },
      .policy = training::trusted_training_policy(),
    });
  const auto committed = require_success(telemetry_coordinator.submit(request),
    "best-effort telemetry must not change a committed submission result");
  require(committed.revision == 1 && submissions == 1 &&
            require_success(telemetry_store.list(), "Store list failed").size() == 1,
    "observer failures are isolated after the Store commit");
}

void validates_custom_store_success_values_and_provider_identity() {
  const auto request = request_for(sample_dataset().manifest);
  const auto provider = std::make_shared<training::function_training_provider>(
    "test-provider",
    [](const training::training_request&, const training::training_context&) {
      return training::training_provider_snapshot {
        .provider_job_id = "custom-store-provider-job",
        .status = training::training_job_status::queued,
      };
    },
    [](const std::string& id, const training::training_context&) {
      return training::training_provider_snapshot {
        .provider_job_id = id,
        .status = training::training_job_status::running,
        .progress = 0.5,
        .current_step = 50,
        .total_steps = 100,
      };
    },
    [](const std::string& id, const training::training_context&) {
      return training::training_provider_snapshot { .provider_job_id = id,
        .status = training::training_job_status::cancelled };
    });

  scripted_training_job_store wrong_scope_store;
  wrong_scope_store.find_fn = [request](
                                const std::string&, const std::string&, const std::string&) {
    return training::training_result<std::optional<training::training_job>>::success(
      persisted_job(request, "wrong-scope-job", "test-provider", "other-tenant", "workspace"));
  };
  training::training_coordinator wrong_scope(provider,
    wrong_scope_store,
    {
      .policy = training::trusted_training_policy(),
    });
  require_error(wrong_scope.submit(request, { .tenant_id = "tenant", .workspace_id = "workspace" }),
    training::training_errc::corrupted_state,
    "custom Store idempotency success cannot escape the requested tenant scope");

  scripted_training_job_store wrong_id_store;
  wrong_id_store.load_fn = [request](const std::string&) {
    return training::training_result<std::optional<training::training_job>>::success(
      persisted_job(request, "different-id"));
  };
  training::training_coordinator wrong_id(provider,
    wrong_id_store,
    {
      .policy = training::trusted_training_policy(),
    });
  const auto wrong_id_result = wrong_id.load("requested-id");
  require(wrong_id_result.error_if() &&
            wrong_id_result.error_if()->code == training::training_errc::corrupted_state,
    "custom Store load success must return the requested job identity");

  scripted_training_job_store altered_create_store;
  altered_create_store.create_fn = [](training::training_job value) {
    value.provider = "substituted-provider";
    value.revision = 1;
    value.updated_at = std::chrono::system_clock::now();
    return training::training_result<training::training_job_create_result>::success(
      { .job = std::move(value), .inserted = true });
  };
  training::training_coordinator altered_create(provider,
    altered_create_store,
    {
      .policy = training::trusted_training_policy(),
    });
  require_error(altered_create.submit(request),
    training::training_errc::reconciliation_required,
    "custom Store create success cannot substitute persisted job ownership after remote submit");

  scripted_training_job_store wrong_revision_store;
  training::training_coordinator wrong_revision(provider,
    wrong_revision_store,
    {
      .policy = training::trusted_training_policy(),
    });
  const auto revision_job = require_success(wrong_revision.submit(request), "submit failed");
  wrong_revision_store.update_fn = [](training::training_job value, std::uint64_t expected) {
    value.revision = expected + 2;
    value.updated_at = std::chrono::system_clock::now();
    return training::training_result<training::training_job>::success(std::move(value));
  };
  require_error(wrong_revision.refresh(revision_job.id),
    training::training_errc::corrupted_state,
    "custom Store update success must advance exactly one revision");

  scripted_training_job_store altered_snapshot_store;
  training::training_coordinator altered_snapshot(provider,
    altered_snapshot_store,
    {
      .policy = training::trusted_training_policy(),
    });
  const auto snapshot_job = require_success(altered_snapshot.submit(request), "submit failed");
  altered_snapshot_store.update_fn = [](training::training_job value, std::uint64_t expected) {
    value.provider_state.progress = 0.75;
    value.revision = expected + 1;
    value.updated_at = std::chrono::system_clock::now();
    return training::training_result<training::training_job>::success(std::move(value));
  };
  require_error(altered_snapshot.refresh(snapshot_job.id),
    training::training_errc::corrupted_state,
    "custom Store update success must persist the coordinator's exact merged snapshot");

  auto changing_provider = std::make_shared<changing_name_training_provider>("frozen-provider");
  training::in_memory_training_job_store frozen_store;
  training::training_coordinator frozen(changing_provider,
    frozen_store,
    {
      .policy = training::trusted_training_policy(),
    });
  const auto frozen_job = require_success(frozen.submit(request), "submit failed");
  const auto refreshed = require_success(frozen.refresh(frozen_job.id), "refresh failed");
  require(frozen_job.provider == "frozen-provider" && refreshed.provider == "frozen-provider" &&
            changing_provider->name_calls == 1,
    "Provider identity is captured exactly once and remains stable for coordinator ownership");

  auto empty_provider = std::make_shared<changing_name_training_provider>("");
  training::in_memory_training_job_store empty_store;
  require_invalid([&] { training::training_coordinator invalid(empty_provider, empty_store); },
    "coordinators reject an empty Provider identity at construction");

  scripted_training_job_store concurrent_store;
  concurrent_store.create_fn = [request](training::training_job value) {
    auto existing = persisted_job(request, "concurrent-existing", "test-provider");
    existing.provider_state.provider_job_id = value.provider_state.provider_job_id;
    return training::training_result<training::training_job_create_result>::success(
      { .job = std::move(existing), .inserted = false });
  };
  auto running_submit_provider = std::make_shared<training::function_training_provider>(
    "test-provider",
    [](const training::training_request&, const training::training_context&) {
      return training::training_provider_snapshot {
        .provider_job_id = "concurrent-provider-job",
        .status = training::training_job_status::running,
        .progress = 0.5,
        .current_step = 50,
        .total_steps = 100,
      };
    },
    [](const std::string& id, const training::training_context&) {
      return training::training_provider_snapshot { .provider_job_id = id,
        .status = training::training_job_status::running };
    },
    [](const std::string& id, const training::training_context&) {
      return training::training_provider_snapshot { .provider_job_id = id,
        .status = training::training_job_status::cancelled };
    });
  training::training_coordinator concurrent(running_submit_provider,
    concurrent_store,
    {
      .policy = training::trusted_training_policy(),
    });
  const auto concurrent_job =
    require_success(concurrent.submit(request), "concurrent submit failed");
  require(concurrent_job.id == "concurrent-existing" &&
            concurrent_job.provider_state.status == training::training_job_status::queued,
    "idempotent concurrent create returns the already persisted job without assuming state order");
}

void reconciles_remote_lifecycle_side_effects() {
  const auto request = request_for(sample_dataset().manifest);
  auto provider = std::make_shared<training::function_training_provider>(
    "side-effect-provider",
    [](const training::training_request&, const training::training_context&) {
      return training::training_provider_snapshot {
        .provider_job_id = "side-effect-job",
        .status = training::training_job_status::paused,
        .current_step = 10,
        .latest_checkpoint =
          training::training_checkpoint {
            .id = "checkpoint-10",
            .uri = "s3://checkpoints/10",
            .digest = digest('e'),
            .step = 10,
          },
      };
    },
    [](const std::string& id, const training::training_context&) {
      return training::training_provider_snapshot {
        .provider_job_id = id, .status = training::training_job_status::paused, .current_step = 10
      };
    },
    [](const std::string& id, const training::training_context&) {
      return training::training_provider_snapshot { .provider_job_id = id,
        .status = training::training_job_status::cancelled,
        .current_step = 10 };
    },
    [](const std::string& id,
      const training::training_checkpoint&,
      const training::training_context&) {
      return training::training_provider_snapshot {
        .provider_job_id = id, .status = training::training_job_status::running, .current_step = 10
      };
    });

  scripted_training_job_store cancel_store;
  training::training_coordinator cancel_coordinator(provider,
    cancel_store,
    {
      .policy = training::trusted_training_policy(),
    });
  const auto cancel_job = require_success(cancel_coordinator.submit(request), "submit failed");
  cancel_store.update_fn = [](training::training_job, std::uint64_t) {
    return training::training_result<training::training_job>::failure({
      .code = training::training_errc::persistence_failure,
      .message = "injected cancel persistence failure",
    });
  };
  const auto cancelled = cancel_coordinator.cancel(cancel_job.id);
  require(cancelled.error_if() &&
            cancelled.error_if()->code == training::training_errc::reconciliation_required &&
            cancelled.error_if()->details.at("operation") == "training.cancel" &&
            cancelled.error_if()->details.at("local_revision") == 1,
    "remote cancel success followed by local failure exposes reconciliation coordinates");

  scripted_training_job_store resume_store;
  training::training_coordinator resume_coordinator(provider,
    resume_store,
    {
      .policy = training::trusted_training_policy(),
    });
  const auto resume_job = require_success(resume_coordinator.submit(request), "submit failed");
  resume_store.update_fn = [](training::training_job value, std::uint64_t expected) {
    value.provider_state.provider_job_id = "corrupted-provider-job";
    value.revision = expected + 1;
    return training::training_result<training::training_job>::success(std::move(value));
  };
  const auto resumed = resume_coordinator.resume(resume_job.id);
  require(resumed.error_if() &&
            resumed.error_if()->code == training::training_errc::reconciliation_required &&
            resumed.error_if()->details.at("operation") == "training.resume",
    "remote resume success followed by a malformed Store success requires reconciliation");
}

void validates_step_bounds_and_frozen_total_steps() {
  training::training_provider_snapshot metric_ahead {
    .provider_job_id = "metric-ahead",
    .status = training::training_job_status::running,
    .current_step = 10,
    .metrics = { { .step = 11, .name = "train.loss", .value = 0.5 } },
  };
  require_invalid([&] { training::validate_provider_snapshot(metric_ahead); },
    "a provider metric cannot report a future step");

  const auto request = request_for(sample_dataset().manifest);
  auto previous = persisted_job(request);
  previous.provider_state.total_steps = 100;
  auto missing = previous;
  missing.provider_state.total_steps.reset();
  require_invalid([&] { training::validate_training_job_update(previous, missing); },
    "persisted total steps cannot disappear");
  auto changed = previous;
  changed.provider_state.total_steps = 200;
  require_invalid([&] { training::validate_training_job_update(previous, changed); },
    "persisted total steps cannot change");

  std::size_t inspections {};
  auto provider = std::make_shared<training::function_training_provider>(
    "total-steps-provider",
    [](const training::training_request&, const training::training_context&) {
      return training::training_provider_snapshot {
        .provider_job_id = "total-steps-job",
        .status = training::training_job_status::queued,
        .total_steps = 100,
      };
    },
    [&inspections](const std::string& id, const training::training_context&) {
      ++inspections;
      return training::training_provider_snapshot {
        .provider_job_id = id,
        .status = training::training_job_status::running,
        .total_steps =
          inspections == 1 ? std::optional<std::size_t> {} : std::optional<std::size_t> { 200 },
      };
    },
    [](const std::string& id, const training::training_context&) {
      return training::training_provider_snapshot { .provider_job_id = id,
        .status = training::training_job_status::cancelled };
    });
  training::in_memory_training_job_store store;
  training::training_coordinator coordinator(provider,
    store,
    {
      .policy = training::trusted_training_policy(),
    });
  const auto job = require_success(coordinator.submit(request), "submit failed");
  require_error(coordinator.refresh(job.id),
    training::training_errc::provider_protocol,
    "a Provider cannot make declared total steps disappear");
  require_error(coordinator.refresh(job.id),
    training::training_errc::provider_protocol,
    "a Provider cannot change declared total steps");

  auto metric_provider = std::make_shared<training::function_training_provider>(
    "metric-step-provider",
    [](const training::training_request&, const training::training_context&) {
      return training::training_provider_snapshot {
        .provider_job_id = "metric-step-job",
        .status = training::training_job_status::queued,
        .current_step = 10,
        .metrics = { { .step = 11, .name = "train.loss", .value = 0.5 } },
      };
    },
    [](const std::string& id, const training::training_context&) {
      return training::training_provider_snapshot { .provider_job_id = id,
        .status = training::training_job_status::running };
    },
    [](const std::string& id, const training::training_context&) {
      return training::training_provider_snapshot { .provider_job_id = id,
        .status = training::training_job_status::cancelled };
    });
  training::in_memory_training_job_store metric_store;
  training::training_coordinator metric_coordinator(metric_provider,
    metric_store,
    {
      .policy = training::trusted_training_policy(),
    });
  require_error(metric_coordinator.submit(request),
    training::training_errc::reconciliation_required,
    "future-step metrics in a successful submit response require remote reconciliation");
}

void coordinates_idempotent_training_and_model_promotion() {
  const auto dataset = sample_dataset();
  auto request = request_for(dataset.manifest);
  struct provider_state {
    std::size_t submissions {};
    std::size_t inspections {};
    training::training_request request;
  };
  auto state = std::make_shared<provider_state>();
  auto provider = std::make_shared<training::function_training_provider>(
    "test-provider",
    [state](const training::training_request& value, const training::training_context&) {
      ++state->submissions;
      state->request = value;
      return training::training_provider_snapshot {
        .provider_job_id = "remote-job-1",
        .status = training::training_job_status::queued,
      };
    },
    [state](const std::string& id, const training::training_context&) {
      ++state->inspections;
      if (state->inspections == 1) {
        return training::training_provider_snapshot {
          .provider_job_id = id,
          .status = training::training_job_status::running,
          .progress = 0.5,
          .current_step = 50,
          .total_steps = 100,
          .metrics = { { .step = 50, .name = "train.loss", .value = 0.42 } },
        };
      }
      return training::training_provider_snapshot {
        .provider_job_id = id,
        .status = training::training_job_status::succeeded,
        .progress = 1.0,
        .current_step = 100,
        .total_steps = 100,
        .artifact = artifact_for(state->request, id),
      };
    },
    [](const std::string& id, const training::training_context&) {
      return training::training_provider_snapshot {
        .provider_job_id = id,
        .status = training::training_job_status::cancelled,
      };
    });

  training::in_memory_training_job_store store;
  training::training_coordinator coordinator(provider,
    store,
    {
      .policy = training::trusted_training_policy(),
    });
  const auto submitted = require_success(coordinator.submit(request), "submit failed");
  const auto duplicate = require_success(coordinator.submit(request), "duplicate submit failed");
  require(submitted.id == duplicate.id && state->submissions == 1 && submitted.revision == 1,
    "idempotent training submission reuses the durable local job");

  auto changed = request;
  changed.hyperparameters.learning_rate = 9e-4;
  require_error(coordinator.submit(changed),
    training::training_errc::invalid_request,
    "an idempotency key cannot conceal a changed training request");

  const auto running = require_success(coordinator.refresh(submitted.id), "refresh failed");
  const auto completed = require_success(coordinator.refresh(submitted.id), "refresh failed");
  require(running.provider_state.status == training::training_job_status::running &&
            running.revision == 2 && completed.revision == 3 &&
            completed.provider_state.status == training::training_job_status::succeeded,
    "coordinator persists monotonic provider state and revisions");

  const auto candidate = learning::learning_candidate_from_training_job(completed, "adapter-v1");
  require(candidate.kind == learning::learning_change_kind::model &&
            candidate.target == request.target && candidate.proposed_version == "adapter-v2" &&
            candidate.parent_version == "adapter-v1" &&
            candidate.artifact.at("dataset").at("digest") == dataset.manifest.digest,
    "successful training artifacts become fully attributable model candidates");
}

void rejects_invalid_provider_state_and_artifact_lineage() {
  const auto request = request_for(sample_dataset().manifest);
  auto bad_artifact = artifact_for(request);
  bad_artifact.dataset_digest = digest('d');
  auto provider = std::make_shared<training::function_training_provider>(
    "test-provider",
    [](const training::training_request&, const training::training_context&) {
      return training::training_provider_snapshot {
        .provider_job_id = "remote-job-1",
        .status = training::training_job_status::running,
        .progress = 0.75,
      };
    },
    [bad_artifact](const std::string& id, const training::training_context&) mutable {
      bad_artifact.provider_job_id = id;
      return training::training_provider_snapshot {
        .provider_job_id = id,
        .status = training::training_job_status::succeeded,
        .progress = 1.0,
        .artifact = bad_artifact,
      };
    },
    [](const std::string& id, const training::training_context&) {
      return training::training_provider_snapshot {
        .provider_job_id = id,
        .status = training::training_job_status::cancelled,
      };
    });
  training::in_memory_training_job_store store;
  training::training_coordinator coordinator(provider,
    store,
    {
      .policy = training::trusted_training_policy(),
    });
  const auto submitted = require_success(coordinator.submit(request), "submit failed");
  require_error(coordinator.refresh(submitted.id),
    training::training_errc::provider_protocol,
    "coordinator rejects artifacts with mismatched dataset lineage");
  require(require_success(store.load(submitted.id), "store load failed")->revision == 1 &&
            require_success(store.load(submitted.id), "store load failed")->provider_state.status ==
              training::training_job_status::running,
    "invalid provider responses do not corrupt the last durable job state");

  auto stale = *require_success(store.load(submitted.id), "store load failed");
  stale.provider_state.progress = 0.9;
  const auto updated = require_success(store.update(stale, stale.revision), "store update failed");
  require_error(store.update(updated, 1),
    training::training_errc::revision_conflict,
    "training job stores reject stale writers by revision");
}

void resumes_only_from_an_attributable_checkpoint() {
  const auto request = request_for(sample_dataset().manifest);
  auto provider = std::make_shared<training::function_training_provider>(
    "test-provider",
    [](const training::training_request&, const training::training_context&) {
      return training::training_provider_snapshot {
        .provider_job_id = "remote-job-1",
        .status = training::training_job_status::queued,
      };
    },
    [](const std::string& id, const training::training_context&) {
      return training::training_provider_snapshot {
        .provider_job_id = id,
        .status = training::training_job_status::paused,
        .progress = 0.4,
        .current_step = 40,
        .total_steps = 100,
        .latest_checkpoint =
          training::training_checkpoint {
            .id = "checkpoint-40",
            .uri = "s3://checkpoints/40",
            .digest = digest('e'),
            .step = 40,
          },
      };
    },
    [](const std::string& id, const training::training_context&) {
      return training::training_provider_snapshot {
        .provider_job_id = id,
        .status = training::training_job_status::cancelled,
      };
    },
    [](const std::string& id,
      const training::training_checkpoint& checkpoint,
      const training::training_context&) {
      require(checkpoint.id == "checkpoint-40", "resume forwards the recorded checkpoint");
      return training::training_provider_snapshot {
        .provider_job_id = id,
        .status = training::training_job_status::running,
        .progress = 0.4,
        .current_step = 40,
        .total_steps = 100,
        .latest_checkpoint = checkpoint,
      };
    });
  training::in_memory_training_job_store store;
  training::training_coordinator coordinator(provider,
    store,
    {
      .policy = training::trusted_training_policy(),
    });
  const auto submitted = require_success(coordinator.submit(request), "submit failed");
  require_error(coordinator.resume(submitted.id),
    training::training_errc::invalid_transition,
    "queued jobs cannot bypass the provider lifecycle through resume");
  const auto paused = require_success(coordinator.refresh(submitted.id), "refresh failed");
  const auto resumed = require_success(coordinator.resume(paused.id), "resume failed");
  require(paused.provider_state.status == training::training_job_status::paused &&
            resumed.provider_state.status == training::training_job_status::running &&
            resumed.revision == 3,
    "paused training resumes from its persisted checkpoint");

  auto no_resume_provider = std::make_shared<training::function_training_provider>(
    "no-resume-provider",
    [](const training::training_request&, const training::training_context&) {
      return training::training_provider_snapshot {
        .provider_job_id = "no-resume-job",
        .status = training::training_job_status::queued,
      };
    },
    [](const std::string& id, const training::training_context&) {
      return training::training_provider_snapshot {
        .provider_job_id = id,
        .status = training::training_job_status::paused,
        .progress = 0.4,
        .current_step = 40,
        .total_steps = 100,
        .latest_checkpoint =
          training::training_checkpoint {
            .id = "checkpoint-40",
            .uri = "s3://checkpoints/40",
            .digest = digest('e'),
            .step = 40,
          },
      };
    },
    [](const std::string& id, const training::training_context&) {
      return training::training_provider_snapshot {
        .provider_job_id = id,
        .status = training::training_job_status::cancelled,
      };
    });
  training::in_memory_training_job_store no_resume_store;
  training::training_coordinator no_resume_coordinator(no_resume_provider,
    no_resume_store,
    {
      .policy = training::trusted_training_policy(),
    });
  const auto no_resume_submitted =
    require_success(no_resume_coordinator.submit(request), "submit without resume support failed");
  const auto no_resume_paused = require_success(
    no_resume_coordinator.refresh(no_resume_submitted.id), "pause observation failed");
  const auto unsupported = no_resume_coordinator.resume(no_resume_paused.id);
  require(
    unsupported.error_if() &&
      unsupported.error_if()->code == training::training_errc::unsupported_operation &&
      unsupported.error_if()->remote_outcome == training::training_remote_outcome::not_applied,
    "an unimplemented resume operation remains an authoritative unsupported result");
}

void dispatches_each_lifecycle_operation_exactly_once() {
  const auto request = request_for(sample_dataset().manifest);
  const auto checkpoint_time = std::chrono::system_clock::time_point(std::chrono::seconds(100));
  struct call_counts {
    std::size_t inspections {};
    std::size_t cancellations {};
    std::size_t resumptions {};
  };
  auto calls = std::make_shared<call_counts>();
  auto provider = std::make_shared<training::function_training_provider>(
    "test-provider",
    [](const training::training_request&, const training::training_context&) {
      return training::training_provider_snapshot {
        .provider_job_id = "remote-job-dispatch",
        .status = training::training_job_status::queued,
      };
    },
    [calls, checkpoint_time](const std::string& id, const training::training_context&) {
      ++calls->inspections;
      return training::training_provider_snapshot {
        .provider_job_id = id,
        .status = training::training_job_status::paused,
        .progress = 0.4,
        .current_step = 40,
        .total_steps = 100,
        .latest_checkpoint =
          training::training_checkpoint {
            .id = "checkpoint-40",
            .uri = "s3://checkpoints/40",
            .digest = digest('e'),
            .step = 40,
            .created_at = checkpoint_time,
          },
      };
    },
    [calls](const std::string& id, const training::training_context&) {
      ++calls->cancellations;
      return training::training_provider_snapshot {
        .provider_job_id = id,
        .status = training::training_job_status::cancelled,
        .progress = 0.4,
        .current_step = 40,
        .total_steps = 100,
      };
    },
    [calls](const std::string& id,
      const training::training_checkpoint& checkpoint,
      const training::training_context&) {
      ++calls->resumptions;
      return training::training_provider_snapshot {
        .provider_job_id = id,
        .status = training::training_job_status::running,
        .progress = 0.4,
        .current_step = checkpoint.step,
        .total_steps = 100,
        .latest_checkpoint = checkpoint,
      };
    });
  training::in_memory_training_job_store store;
  training::training_coordinator coordinator(provider,
    store,
    {
      .policy = training::trusted_training_policy(),
    });
  const auto submitted = require_success(coordinator.submit(request), "submit failed");
  const auto paused = require_success(coordinator.refresh(submitted.id), "refresh failed");
  const auto still_paused = require_success(coordinator.refresh(paused.id), "refresh failed");
  require(paused.provider_state.status == training::training_job_status::paused &&
            still_paused.provider_state.status == training::training_job_status::paused &&
            calls->inspections == 2 && calls->resumptions == 0,
    "refresh inspects paused jobs without implicitly resuming them");

  const auto cancelled = require_success(coordinator.cancel(paused.id), "cancel failed");
  require(cancelled.provider_state.status == training::training_job_status::cancelled &&
            calls->cancellations == 1 && calls->resumptions == 0,
    "cancel dispatches only the provider cancellation operation");
  const auto terminal_refresh =
    require_success(coordinator.refresh(cancelled.id), "terminal refresh failed");
  require(terminal_refresh.revision == cancelled.revision && calls->inspections == 2,
    "refresh of a terminal job is an idempotent local read");
  require_error(coordinator.cancel(cancelled.id),
    training::training_errc::invalid_transition,
    "terminal jobs cannot be cancelled again");
  require_error(coordinator.resume(cancelled.id),
    training::training_errc::invalid_transition,
    "terminal jobs cannot be resumed");
  require(calls->cancellations == 1 && calls->resumptions == 0,
    "invalid terminal operations do not reach the provider");
}

void classifies_resume_exceptions_and_provider_history_regressions() {
  const auto request = request_for(sample_dataset().manifest);
  std::size_t inspections {};
  auto provider = std::make_shared<training::function_training_provider>(
    "test-provider",
    [](const training::training_request&, const training::training_context&) {
      return training::training_provider_snapshot {
        .provider_job_id = "remote-job-regression",
        .status = training::training_job_status::queued,
      };
    },
    [&inspections](const std::string& id, const training::training_context&) {
      ++inspections;
      if (inspections == 1) {
        return training::training_provider_snapshot {
          .provider_job_id = id,
          .status = training::training_job_status::paused,
          .progress = 0.5,
          .current_step = 50,
          .total_steps = 100,
          .metrics = { { .step = 50,
            .name = "train.loss",
            .value = 0.5,
            .metadata = { { "split", "train" } } } },
          .latest_checkpoint =
            training::training_checkpoint {
              .id = "checkpoint-50",
              .uri = "s3://checkpoints/50",
              .digest = digest('f'),
              .step = 50,
            },
        };
      }
      return training::training_provider_snapshot {
        .provider_job_id = id,
        .status = training::training_job_status::running,
        .progress = 0.6,
        .current_step = 40,
        .total_steps = 100,
      };
    },
    [](const std::string& id, const training::training_context&) {
      return training::training_provider_snapshot { .provider_job_id = id,
        .status = training::training_job_status::cancelled };
    },
    [](const std::string&,
      const training::training_checkpoint&,
      const training::training_context&) -> training::training_provider_snapshot {
      throw std::runtime_error("resume transport failed");
    });
  training::in_memory_training_job_store store;
  training::training_coordinator coordinator(provider,
    store,
    {
      .policy = training::trusted_training_policy(),
    });
  const auto submitted = require_success(coordinator.submit(request), "submit failed");
  const auto paused = require_success(coordinator.refresh(submitted.id), "refresh failed");
  const auto ambiguous_resume = coordinator.resume(paused.id);
  require(
    ambiguous_resume.error_if() &&
      ambiguous_resume.error_if()->code == training::training_errc::reconciliation_required &&
      ambiguous_resume.error_if()->remote_outcome == training::training_remote_outcome::uncertain,
    "exceptions from provider resume require reconciliation because the remote result is unknown");
  require_error(coordinator.refresh(paused.id),
    training::training_errc::provider_protocol,
    "provider current-step rollback is a protocol failure");

  std::size_t metric_inspections {};
  auto metric_provider = std::make_shared<training::function_training_provider>(
    "metric-provider",
    [](const training::training_request&, const training::training_context&) {
      return training::training_provider_snapshot {
        .provider_job_id = "remote-job-metric",
        .status = training::training_job_status::queued,
      };
    },
    [&metric_inspections](const std::string& id, const training::training_context&) {
      ++metric_inspections;
      return training::training_provider_snapshot {
        .provider_job_id = id,
        .status = training::training_job_status::running,
        .progress = metric_inspections == 1 ? 0.5 : 0.6,
        .current_step = metric_inspections == 1 ? 50u : 60u,
        .total_steps = 100,
        .metrics = { { .step = 50,
          .name = "train.loss",
          .value = metric_inspections == 1 ? 0.5 : 0.4,
          .metadata = { { "split", "train" } } } },
      };
    },
    [](const std::string& id, const training::training_context&) {
      return training::training_provider_snapshot { .provider_job_id = id,
        .status = training::training_job_status::cancelled };
    });
  training::in_memory_training_job_store metric_store;
  training::training_coordinator metric_coordinator(metric_provider,
    metric_store,
    {
      .policy = training::trusted_training_policy(),
    });
  const auto metric_job = require_success(metric_coordinator.submit(request), "submit failed");
  (void)require_success(metric_coordinator.refresh(metric_job.id), "refresh failed");
  require_error(metric_coordinator.refresh(metric_job.id),
    training::training_errc::provider_protocol,
    "rewriting an observed metric is a provider protocol failure");
}

void rejects_ambiguous_or_rewritten_provider_history() {
  const auto request = request_for(sample_dataset().manifest);
  auto duplicate_metric_provider = std::make_shared<training::function_training_provider>(
    "duplicate-metric-provider",
    [](const training::training_request&, const training::training_context&) {
      return training::training_provider_snapshot {
        .provider_job_id = "remote-duplicate-metric",
        .status = training::training_job_status::queued,
      };
    },
    [](const std::string& id, const training::training_context&) {
      return training::training_provider_snapshot {
        .provider_job_id = id,
        .status = training::training_job_status::running,
        .progress = 0.5,
        .current_step = 50,
        .total_steps = 100,
        .metrics = {
          { .step = 50, .name = "train.loss", .value = 0.5 },
          { .step = 50, .name = "train.loss", .value = 0.4 },
        },
      };
    },
    [](const std::string& id, const training::training_context&) {
      return training::training_provider_snapshot { .provider_job_id = id,
        .status = training::training_job_status::cancelled };
    });
  training::in_memory_training_job_store duplicate_store;
  training::training_coordinator duplicate_coordinator(
    duplicate_metric_provider, duplicate_store, { .policy = training::trusted_training_policy() });
  const auto duplicate_job =
    require_success(duplicate_coordinator.submit(request), "submit failed");
  require_error(duplicate_coordinator.refresh(duplicate_job.id),
    training::training_errc::provider_protocol,
    "provider snapshots reject duplicate metric identities");

  const auto metric_time = std::chrono::system_clock::time_point(std::chrono::seconds(200));
  std::size_t timestamp_inspections {};
  auto timestamp_provider = std::make_shared<training::function_training_provider>(
    "timestamp-provider",
    [](const training::training_request&, const training::training_context&) {
      return training::training_provider_snapshot {
        .provider_job_id = "remote-metric-timestamp",
        .status = training::training_job_status::queued,
      };
    },
    [&timestamp_inspections, metric_time](
      const std::string& id, const training::training_context&) {
      ++timestamp_inspections;
      return training::training_provider_snapshot {
        .provider_job_id = id,
        .status = training::training_job_status::running,
        .progress = timestamp_inspections == 1 ? 0.5 : 0.6,
        .current_step = timestamp_inspections == 1 ? 50u : 60u,
        .total_steps = 100,
        .metrics = { { .step = 50,
          .name = "train.loss",
          .value = 0.5,
          .observed_at = metric_time + std::chrono::seconds(timestamp_inspections - 1) } },
      };
    },
    [](const std::string& id, const training::training_context&) {
      return training::training_provider_snapshot { .provider_job_id = id,
        .status = training::training_job_status::cancelled };
    });
  training::in_memory_training_job_store timestamp_store;
  training::training_coordinator timestamp_coordinator(
    timestamp_provider, timestamp_store, { .policy = training::trusted_training_policy() });
  const auto timestamp_job =
    require_success(timestamp_coordinator.submit(request), "submit failed");
  (void)require_success(timestamp_coordinator.refresh(timestamp_job.id), "refresh failed");
  require_error(timestamp_coordinator.refresh(timestamp_job.id),
    training::training_errc::provider_protocol,
    "an observed metric timestamp cannot be rewritten");

  const auto checkpoint_time = std::chrono::system_clock::time_point(std::chrono::seconds(100));
  std::size_t checkpoint_inspections {};
  auto checkpoint_provider = std::make_shared<training::function_training_provider>(
    "checkpoint-provider",
    [](const training::training_request&, const training::training_context&) {
      return training::training_provider_snapshot {
        .provider_job_id = "remote-checkpoint-history",
        .status = training::training_job_status::queued,
      };
    },
    [&checkpoint_inspections, checkpoint_time](
      const std::string& id, const training::training_context&) {
      ++checkpoint_inspections;
      return training::training_provider_snapshot {
        .provider_job_id = id,
        .status = training::training_job_status::paused,
        .progress = 0.5,
        .current_step = 50,
        .total_steps = 100,
        .latest_checkpoint =
          training::training_checkpoint {
            .id = checkpoint_inspections == 1 ? "checkpoint-50" : "replacement-50",
            .uri = "s3://checkpoints/50",
            .digest = digest(checkpoint_inspections == 1 ? 'e' : 'f'),
            .step = 50,
            .created_at = checkpoint_time,
          },
      };
    },
    [](const std::string& id, const training::training_context&) {
      return training::training_provider_snapshot { .provider_job_id = id,
        .status = training::training_job_status::cancelled };
    });
  training::in_memory_training_job_store checkpoint_store;
  training::training_coordinator checkpoint_coordinator(
    checkpoint_provider, checkpoint_store, { .policy = training::trusted_training_policy() });
  const auto checkpoint_job =
    require_success(checkpoint_coordinator.submit(request), "submit failed");
  (void)require_success(checkpoint_coordinator.refresh(checkpoint_job.id), "refresh failed");
  require_error(checkpoint_coordinator.refresh(checkpoint_job.id),
    training::training_errc::provider_protocol,
    "a checkpoint cannot be replaced at an already observed step");

  std::size_t status_inspections {};
  auto status_provider = std::make_shared<training::function_training_provider>(
    "status-provider",
    [](const training::training_request&, const training::training_context&) {
      return training::training_provider_snapshot {
        .provider_job_id = "remote-status-history",
        .status = training::training_job_status::queued,
      };
    },
    [&status_inspections](const std::string& id, const training::training_context&) {
      ++status_inspections;
      return training::training_provider_snapshot {
        .provider_job_id = id,
        .status = status_inspections == 1 ? training::training_job_status::running
                                          : training::training_job_status::queued,
      };
    },
    [](const std::string& id, const training::training_context&) {
      return training::training_provider_snapshot { .provider_job_id = id,
        .status = training::training_job_status::cancelled };
    });
  training::in_memory_training_job_store status_store;
  training::training_coordinator status_coordinator(
    status_provider, status_store, { .policy = training::trusted_training_policy() });
  const auto status_job = require_success(status_coordinator.submit(request), "submit failed");
  (void)require_success(status_coordinator.refresh(status_job.id), "refresh failed");
  require_error(status_coordinator.refresh(status_job.id),
    training::training_errc::provider_protocol,
    "provider status rollback is classified as a provider protocol failure");
}

void isolates_training_jobs_by_tenant_and_workspace() {
  const auto request = request_for(sample_dataset().manifest);
  struct provider_calls {
    std::size_t submissions {};
    std::size_t inspections {};
    std::size_t cancellations {};
  };
  auto calls = std::make_shared<provider_calls>();
  auto provider = std::make_shared<training::function_training_provider>(
    "test-provider",
    [calls](const training::training_request&, const training::training_context&) {
      ++calls->submissions;
      return training::training_provider_snapshot {
        .provider_job_id = "remote-scope-" + std::to_string(calls->submissions),
        .status = training::training_job_status::queued,
      };
    },
    [calls](const std::string& id, const training::training_context&) {
      ++calls->inspections;
      return training::training_provider_snapshot { .provider_job_id = id,
        .status = training::training_job_status::running };
    },
    [calls](const std::string& id, const training::training_context&) {
      ++calls->cancellations;
      return training::training_provider_snapshot { .provider_job_id = id,
        .status = training::training_job_status::cancelled };
    });
  training::in_memory_training_job_store store;
  training::training_coordinator coordinator(provider,
    store,
    {
      .policy = training::trusted_training_policy(),
    });
  const training::training_context tenant_a { .tenant_id = "tenant-a",
    .workspace_id = "workspace-a" };
  const training::training_context tenant_b { .tenant_id = "tenant-b",
    .workspace_id = "workspace-b" };
  const auto first = require_success(coordinator.submit(request, tenant_a), "submit failed");
  const auto duplicate = require_success(coordinator.submit(request, tenant_a), "submit failed");
  const auto second = require_success(coordinator.submit(request, tenant_b), "submit failed");
  require(first.id == duplicate.id && first.id != second.id && calls->submissions == 2 &&
            first.tenant_id == "tenant-a" && first.workspace_id == "workspace-a" &&
            second.tenant_id == "tenant-b" && second.workspace_id == "workspace-b",
    "idempotency keys are independent across tenant and workspace scopes");
  require(!require_success(coordinator.load(first.id, tenant_b), "load failed") &&
            require_success(coordinator.load(first.id, tenant_a), "load failed")->id == first.id,
    "cross-tenant loads do not disclose a job");
  require_error(coordinator.refresh(first.id, tenant_b),
    training::training_errc::not_found,
    "cross-tenant refresh is rejected as not found");
  require_error(coordinator.cancel(first.id, tenant_b),
    training::training_errc::not_found,
    "cross-tenant cancel is rejected as not found");
  require_error(coordinator.resume(first.id, tenant_b),
    training::training_errc::not_found,
    "cross-tenant resume is rejected as not found");
  require(calls->inspections == 0 && calls->cancellations == 0,
    "cross-scope operations never reach the provider");
}

void rejects_cross_provider_idempotency_reuse() {
  const auto request = request_for(sample_dataset().manifest);
  const auto make_provider = [](std::string name, std::string remote_id) {
    return std::make_shared<training::function_training_provider>(
      std::move(name),
      [remote_id = std::move(remote_id)](
        const training::training_request&, const training::training_context&) {
        return training::training_provider_snapshot { .provider_job_id = remote_id,
          .status = training::training_job_status::queued };
      },
      [](const std::string& id, const training::training_context&) {
        return training::training_provider_snapshot { .provider_job_id = id,
          .status = training::training_job_status::running };
      },
      [](const std::string& id, const training::training_context&) {
        return training::training_provider_snapshot { .provider_job_id = id,
          .status = training::training_job_status::cancelled };
      });
  };
  training::in_memory_training_job_store store;
  training::training_coordinator first(make_provider("provider-a", "remote-a"),
    store,
    {
      .policy = training::trusted_training_policy(),
    });
  training::training_coordinator second(make_provider("provider-b", "remote-b"),
    store,
    {
      .policy = training::trusted_training_policy(),
    });
  (void)require_success(first.submit(request), "submit failed");
  require_error(second.submit(request),
    training::training_errc::invalid_request,
    "an idempotency record cannot be silently adopted by a different provider");
}

void http_provider_uses_the_versioned_job_protocol() {
  const auto training_request = request_for(sample_dataset().manifest);
  auto http = std::make_shared<recording_http_client>();
  http->responses.push_back({
    .status_code = 202,
    .body =
      R"({"protocolVersion":"2026-08-01","requestId":"submit-request","operation":"submit","ok":true,"body":{"provider_job_id":"remote/job 1","status":"queued","progress":0,"current_step":0}})",
  });
  http->responses.push_back({
    .status_code = 200,
    .body =
      R"({"protocolVersion":"2026-08-01","requestId":"inspect-request","operation":"inspect","ok":true,"body":{"provider_job_id":"remote/job 1","status":"running","progress":0.25,"current_step":25,"total_steps":100}})",
  });
  http->responses.push_back({
    .status_code = 200,
    .body =
      R"({"protocolVersion":"2026-08-01","requestId":"cancel-request","operation":"cancel","ok":true,"body":{"provider_job_id":"remote/job 1","status":"cancelled","progress":0.25,"current_step":25,"total_steps":100}})",
  });
  training::http_training_provider provider(
    {
      .name = "test-http",
      .base_url = "https://trainer.example/",
      .bearer_token = "secret",
      .timeout_ms = 5000,
      .headers = { { "X-Tenant", "tenant-1" } },
    },
    http);

  const auto submitted = require_success(provider.submit(training_request,
                                           { .trace_id = "trace-1",
                                             .request_id = "submit-request",
                                             .subject_id = "subject-1",
                                             .tenant_id = "tenant-ctx",
                                             .workspace_id = "workspace-1" }),
    "HTTP submit failed");
  const auto running = require_success(
    provider.inspect(submitted.provider_job_id, { .request_id = "inspect-request" }),
    "HTTP inspect failed");
  const auto cancelled =
    require_success(provider.cancel(submitted.provider_job_id, { .request_id = "cancel-request" }),
      "HTTP cancel failed");
  require(running.status == training::training_job_status::running &&
            cancelled.status == training::training_job_status::cancelled,
    "HTTP provider decodes versioned job snapshots");
  require(http->requests.size() == 3 &&
            http->requests[0].url == "https://trainer.example/v1/training/jobs" &&
            http->requests[0].method == "POST" &&
            has_header(http->requests[0], "Idempotency-Key", training_request.idempotency_key) &&
            has_header(http->requests[0], "Authorization", "Bearer secret") &&
            has_header(http->requests[0], "Wuwe-Request-Id", "submit-request") &&
            has_header(http->requests[0], "Wuwe-Training-Operation", "submit") &&
            has_header(http->requests[0], "Wuwe-Trace-Id", "trace-1") &&
            has_header(http->requests[0], "Wuwe-Subject-Id", "subject-1") &&
            has_header(http->requests[0], "Wuwe-Tenant-Id", "tenant-ctx") &&
            has_header(http->requests[0], "Wuwe-Workspace-Id", "workspace-1") &&
            has_header(http->requests[0], "X-Tenant", "tenant-1"),
    "HTTP submission carries authentication, tenancy, and idempotency controls");
  require(!http->requests[0].follow_redirects && http->requests[0].max_redirects == 0,
    "HTTP training requests disable redirects by default to protect credentials and identity");
  require(http->requests[1].method == "GET" &&
            http->requests[1].url.find("remote%2Fjob%201") != std::string::npos &&
            http->requests[2].url.ends_with("/cancel"),
    "HTTP provider URL-encodes opaque provider job ids");

  auto invalid_http = std::make_shared<recording_http_client>();
  invalid_http->responses.push_back({
    .status_code = 200,
    .body =
      R"({"protocolVersion":"2026-08-01","requestId":"invalid-request","operation":"inspect","ok":true,"body":{"provider_job_id":"job","status":"succeeded","progress":1}})",
  });
  training::http_training_provider invalid_provider(
    {
      .base_url = "https://trainer.example",
    },
    invalid_http);
  require_error(invalid_provider.inspect("job", { .request_id = "invalid-request" }),
    training::training_errc::provider_protocol,
    "HTTP provider rejects successful responses without model artifacts");

  const auto submit_envelope = nlohmann::json::parse(http->requests[0].body);
  require(submit_envelope.at("context").at("tenantId") == "tenant-ctx" &&
            submit_envelope.at("context").at("workspaceId") == "workspace-1" &&
            !submit_envelope.at("context").contains("metadata"),
    "HTTP envelopes propagate named context identifiers without arbitrary metadata");
}

void http_provider_preserves_authoritative_responses_after_cancellation() {
  const auto request = request_for(sample_dataset().manifest);
  std::stop_source stop_source;
  auto http = std::make_shared<cancelling_http_client>(stop_source,
    wuwe::http_response {
      .status_code = 200,
      .body =
        R"({"protocolVersion":"2026-08-01","requestId":"cancel-race","operation":"submit","ok":true,"body":{"provider_job_id":"authoritative-job","status":"queued","progress":0.0,"current_step":0}})",
    });
  training::http_training_provider provider({ .base_url = "https://trainer.example" }, http);
  const auto result = require_success(provider.submit(request,
                                        {
                                          .request_id = "cancel-race",
                                          .stop_token = stop_source.get_token(),
                                        }),
    "an authoritative HTTP response must win the cancellation race");
  require(stop_source.stop_requested() && result.provider_job_id == "authoritative-job" &&
            result.status == training::training_job_status::queued,
    "HTTP Provider parses a complete response even if cancellation is observed after send returns");
}

void http_provider_supports_lookup_and_cooperative_cancellation() {
  const auto request = request_for(sample_dataset().manifest);
  auto lookup_http = std::make_shared<recording_http_client>();
  lookup_http->responses.push_back({
    .status_code = 200,
    .body = "{\"protocolVersion\":\"2026-08-01\",\"requestId\":\"lookup-request\","
            "\"operation\":\"lookup\",\"ok\":true,\"body\":{"
            "\"idempotency_key\":\"" +
            request.idempotency_key + "\",\"request_id\":\"" + request.id +
            "\",\"request_digest\":\"" + training::training_request_digest(request) +
            "\",\"snapshot\":{\"provider_job_id\":\"looked-up-job\","
            "\"status\":\"queued\",\"progress\":0.0,\"current_step\":0}}}",
  });
  training::http_training_provider lookup_provider(
    { .base_url = "https://trainer.example" }, lookup_http);
  const auto found = require_success(
    lookup_provider.lookup_submission(request.idempotency_key, { .request_id = "lookup-request" }),
    "HTTP lookup failed");
  require(
    found && found->snapshot.provider_job_id == "looked-up-job" &&
      lookup_http->requests.front().method == "GET" &&
      lookup_http->requests.front().url.find("/v1/training/submissions/") != std::string::npos &&
      has_header(lookup_http->requests.front(), "Idempotency-Key", request.idempotency_key),
    "HTTP Provider looks up submissions through the versioned idempotency endpoint");

  auto malformed_lookup_http = std::make_shared<recording_http_client>();
  malformed_lookup_http->responses.push_back({
    .status_code = 200,
    .body =
      nlohmann::json {
        { "protocolVersion", "2026-08-01" },
        { "requestId", "malformed-lookup" },
        { "operation", "lookup" },
        { "ok", true },
        { "body",
          {
            { "idempotency_key", request.idempotency_key },
            { "request_id", request.id },
            { "request_digest", training::training_request_digest(request) },
            { "snapshot",
              {
                { "provider_job_id", "malformed-lookup-job" },
                { "status", "succeeded" },
                { "progress", 1.0 },
                { "current_step", 100 },
                { "total_steps", 100 },
              } },
          } },
      }
        .dump(),
  });
  auto malformed_lookup_provider = std::make_shared<training::http_training_provider>(
    training::http_training_provider_config { .base_url = "https://trainer.example" },
    malformed_lookup_http);
  training::in_memory_training_job_store malformed_lookup_store;
  training::training_coordinator malformed_lookup_coordinator(malformed_lookup_provider,
    malformed_lookup_store,
    {
      .policy = training::trusted_training_policy(),
    });
  const auto malformed_lookup =
    malformed_lookup_coordinator.reconcile_submit(request, { .request_id = "malformed-lookup" });
  require(
    malformed_lookup.error_if() &&
      malformed_lookup.error_if()->code == training::training_errc::reconciliation_required &&
      malformed_lookup.error_if()->remote_outcome == training::training_remote_outcome::applied &&
      malformed_lookup.error_if()->details.at("provider_job_id") == "malformed-lookup-job" &&
      require_success(malformed_lookup_store.list(), "Store list failed").empty(),
    "a found HTTP submission with a malformed body preserves reconciliation semantics");

  std::stop_source stop_source;
  auto cancelling_http = std::make_shared<cooperatively_cancelling_http_client>();
  cancelling_http->stop_source = &stop_source;
  training::http_training_provider cancelling_provider(
    { .base_url = "https://trainer.example" }, cancelling_http);
  const auto cancelled = cancelling_provider.submit(request,
    {
      .request_id = "cancel-in-flight",
      .stop_token = stop_source.get_token(),
    });
  require(cancelled.error_if() &&
            cancelled.error_if()->code == training::training_errc::cancelled &&
            cancelled.error_if()->remote_outcome == training::training_remote_outcome::uncertain &&
            cancelling_http->plain_sends == 0 && cancelling_http->stream_sends == 1 &&
            cancelling_http->observed_token,
    "HTTP training calls use the cancellable transport path and preserve mutation uncertainty");

  auto oversized_http = std::make_shared<recording_http_client>();
  oversized_http->responses.push_back({
    .status_code = 202,
    .body = std::string(512, 'x'),
  });
  training::http_training_provider oversized_provider(
    {
      .base_url = "https://trainer.example",
      .max_response_bytes = 128,
    },
    oversized_http);
  const auto oversized = oversized_provider.submit(request, { .request_id = "oversized" });
  require(oversized.error_if() &&
            oversized.error_if()->code == training::training_errc::provider_protocol &&
            oversized.error_if()->remote_outcome == training::training_remote_outcome::uncertain,
    "oversized mutation responses are bounded and preserve remote uncertainty");
}

void rejects_inconsistent_errors_and_revision_overflow() {
  require_invalid(
    [] {
      training::validate_training_error({
        .code = training::training_errc::invalid_request,
        .message = "invalid",
        .remote_outcome = training::training_remote_outcome::applied,
      });
    },
    "training errors reject business failures marked as remotely applied");
  require_invalid(
    [] {
      training::validate_training_error({
        .code = training::training_errc::reconciliation_required,
        .message = "reconcile",
        .remote_outcome = training::training_remote_outcome::not_applied,
      });
    },
    "reconciliation errors require applied or uncertain remote outcomes");

  const auto request = request_for(sample_dataset().manifest);
  require(!training::next_training_revision((std::numeric_limits<std::uint64_t>::max)()) &&
            training::next_training_revision(41) == 42,
    "built-in Stores share an overflow-safe revision increment contract");

  class max_revision_store final : public training::training_job_store {
  public:
    explicit max_revision_store(training::training_job job) : job_(std::move(job)) {
    }
    [[nodiscard]] wuwe::agent::core::storage_capabilities capabilities() const noexcept override {
      return { .declared = true,
        .optimistic_concurrency = true,
        .atomic_mutations = true,
        .coordination_scope = wuwe::agent::core::storage_coordination_scope::process_local,
        .schema_version = 1 };
    }
    training::training_result<training::training_job_create_result> create(
      training::training_job) override {
      return training::training_result<training::training_job_create_result>::failure(
        { .code = training::training_errc::invalid_request, .message = "unused" });
    }
    training::training_result<std::optional<training::training_job>> load(
      const std::string& id) const override {
      return training::training_result<std::optional<training::training_job>>::success(
        id == job_.id ? std::optional(job_) : std::nullopt);
    }
    training::training_result<std::optional<training::training_job>> find_by_idempotency_key(
      const std::string&, const std::string&, const std::string&) const override {
      return training::training_result<std::optional<training::training_job>>::success(job_);
    }
    training::training_result<std::vector<training::training_job>> list() const override {
      return training::training_result<std::vector<training::training_job>>::success({ job_ });
    }
    training::training_result<training::training_job> update(
      training::training_job, std::uint64_t expected) override {
      if (expected == (std::numeric_limits<std::uint64_t>::max)())
        return training::training_result<training::training_job>::failure(
          { .code = training::training_errc::revision_conflict,
            .message = "training job revision limit reached" });
      return training::training_result<training::training_job>::failure(
        { .code = training::training_errc::corrupted_state, .message = "unexpected" });
    }

  private:
    training::training_job job_;
  };
  auto job = persisted_job(request, "max-job");
  job.revision = (std::numeric_limits<std::uint64_t>::max)();
  max_revision_store store(job);
  auto provider = std::make_shared<training::function_training_provider>(
    "test-provider",
    [](const training::training_request&, const training::training_context&) {
      return training::training_provider_snapshot { .provider_job_id = "unused",
        .status = training::training_job_status::queued };
    },
    [](const std::string& id, const training::training_context&) {
      return training::training_provider_snapshot { .provider_job_id = id,
        .status = training::training_job_status::running };
    },
    [](const std::string& id, const training::training_context&) {
      return training::training_provider_snapshot { .provider_job_id = id,
        .status = training::training_job_status::cancelled };
    });
  training::training_coordinator coordinator(provider,
    store,
    {
      .policy = training::trusted_training_policy(),
    });
  require_error(coordinator.refresh(job.id),
    training::training_errc::revision_conflict,
    "revision exhaustion fails safely instead of wrapping to zero");
}

void rejects_malformed_http_envelopes() {
  const auto request = request_for(sample_dataset().manifest);
  struct fixture {
    int status;
    const char* body;
  };
  const std::vector<fixture> cases {
    { 200,
      R"({"protocolVersion":"2026-08-01","requestId":"r","operation":"submit","ok":true,"body":{},"error":{"code":"invalid_request","message":"bad"}})" },
    { 200, R"({"protocolVersion":"2026-08-01","requestId":"r","operation":"submit","ok":true})" },
    { 400,
      R"({"protocolVersion":"2026-08-01","requestId":"r","operation":"submit","ok":false,"body":{},"error":{"code":"invalid_request","message":"bad"}})" },
    { 400, R"({"protocolVersion":"2026-08-01","requestId":"r","operation":"submit","ok":false})" },
    { 500,
      R"({"protocolVersion":"2026-08-01","requestId":"r","operation":"submit","ok":true,"body":{}})" },
    { 400,
      R"({"protocolVersion":"2026-08-01","requestId":"r","operation":"submit","ok":false,"error":{"code":"","message":""}})" },
    { 400,
      R"({"protocolVersion":"2026-08-01","requestId":"r","operation":"submit","ok":false,"error":{"code":"invalid_request","message":"bad","details":[]}})" },
  };
  for (const auto& item : cases) {
    auto http = std::make_shared<recording_http_client>();
    http->responses.push_back({ .status_code = item.status, .body = item.body });
    training::http_training_provider provider({ .base_url = "https://trainer.example" }, http);
    require_error(provider.submit(request, { .request_id = "r" }),
      training::training_errc::provider_protocol,
      "HTTP provider must reject malformed or contradictory response envelopes");
  }
}

void classifies_http_errors_and_rejects_unsafe_headers() {
  const auto request = request_for(sample_dataset().manifest);
  auto business_http = std::make_shared<recording_http_client>();
  business_http->responses.push_back({
    .error_code = wuwe::make_error_code(wuwe::http_status_code::bad_request),
    .status_code = 400,
    .body =
      R"({"protocolVersion":"2026-08-01","requestId":"business","operation":"submit","ok":false,"error":{"code":"invalid_request","message":"dataset rejected","retryable":false,"details":{"field":"dataset"}}})",
  });
  training::http_training_provider business_provider(
    { .base_url = "https://trainer.example" }, business_http);
  const auto business = business_provider.submit(request, { .request_id = "business" });
  const auto* business_error = business.error_if();
  require(business_error && business_error->code == training::training_errc::invalid_request &&
            business_error->message == "dataset rejected" &&
            business_error->details.at("field") == "dataset",
    "HTTP 4xx status codes with typed envelopes remain business errors, not transport failures");

  const std::vector<std::pair<std::string, training::training_errc>> mappings {
    { "invalid_request", training::training_errc::invalid_request },
    { "not_found", training::training_errc::not_found },
    { "invalid_transition", training::training_errc::invalid_transition },
    { "revision_conflict", training::training_errc::revision_conflict },
    { "provider_transport", training::training_errc::provider_transport },
    { "provider_protocol", training::training_errc::provider_protocol },
    { "authorization_denied", training::training_errc::authorization_denied },
    { "persistence_failure", training::training_errc::persistence_failure },
    { "corrupted_state", training::training_errc::corrupted_state },
    { "cancelled", training::training_errc::cancelled },
    { "timed_out", training::training_errc::timed_out },
    { "unsupported_operation", training::training_errc::unsupported_operation },
    { "reconciliation_required", training::training_errc::reconciliation_required },
  };
  for (const auto& [code, expected] : mappings) {
    auto http = std::make_shared<recording_http_client>();
    http->responses.push_back({
      .status_code = 400,
      .body =
        nlohmann::json {
          { "protocolVersion", "2026-08-01" },
          { "requestId", "mapping" },
          { "operation", "submit" },
          { "ok", false },
          { "error", { { "code", code }, { "message", "mapped" } } },
        }
          .dump(),
    });
    training::http_training_provider provider({ .base_url = "https://trainer.example" }, http);
    require_error(provider.submit(request, { .request_id = "mapping" }),
      expected,
      "HTTP typed training error code must preserve its public enum mapping");
  }

  auto unknown_http = std::make_shared<recording_http_client>();
  unknown_http->responses.push_back({
    .status_code = 500,
    .body =
      R"({"protocolVersion":"2026-08-01","requestId":"unknown","operation":"submit","ok":false,"error":{"code":"future_error","message":"unknown"}})",
  });
  training::http_training_provider unknown_provider(
    { .base_url = "https://trainer.example" }, unknown_http);
  require_error(unknown_provider.submit(request, { .request_id = "unknown" }),
    training::training_errc::provider_protocol,
    "unknown HTTP error codes are rejected instead of silently changing semantics");

  auto read_only_effect_http = std::make_shared<recording_http_client>();
  read_only_effect_http->responses.push_back({
    .status_code = 500,
    .body =
      R"({"protocolVersion":"2026-08-01","requestId":"read-only-effect","operation":"inspect","ok":false,"error":{"code":"provider_protocol","message":"invalid effect","remoteOutcome":"applied"}})",
  });
  training::http_training_provider read_only_effect_provider(
    { .base_url = "https://trainer.example" }, read_only_effect_http);
  const auto read_only_effect =
    read_only_effect_provider.inspect("job", { .request_id = "read-only-effect" });
  require(read_only_effect.error_if() &&
            read_only_effect.error_if()->code == training::training_errc::provider_protocol &&
            read_only_effect.error_if()->remote_outcome ==
              training::training_remote_outcome::not_applicable,
    "HTTP read-only operations reject response envelopes that claim remote mutation effects");

  auto transport_http = std::make_shared<recording_http_client>();
  transport_http->responses.push_back({
    .transport_error = std::make_error_code(std::errc::connection_reset),
  });
  training::http_training_provider transport_provider(
    { .base_url = "https://trainer.example" }, transport_http);
  require_error(transport_provider.submit(request, { .request_id = "transport" }),
    training::training_errc::provider_transport,
    "explicit HTTP transport failures retain provider-transport semantics");

  auto not_applied_http = std::make_shared<recording_http_client>();
  not_applied_http->responses.push_back({
    .status_code = 503,
    .body =
      R"({"protocolVersion":"2026-08-01","requestId":"not-applied","operation":"submit","ok":false,"error":{"code":"provider_transport","message":"rejected before dispatch","retryable":true,"remoteOutcome":"not_applied"}})",
  });
  auto not_applied_provider = std::make_shared<training::http_training_provider>(
    training::http_training_provider_config { .base_url = "https://trainer.example" },
    not_applied_http);
  training::in_memory_training_job_store not_applied_store;
  training::training_coordinator not_applied_coordinator(
    not_applied_provider, not_applied_store, { .policy = training::trusted_training_policy() });
  require_error(not_applied_coordinator.submit(request, { .request_id = "not-applied" }),
    training::training_errc::provider_transport,
    "an authoritative not_applied outcome preserves the underlying retryable error");

  const std::vector<std::string> reserved_names {
    "authorization",
    "CONTENT-TYPE",
    "idempotency-key",
    "wuwe-request-id",
    "Wuwe-Tenant-Id",
    "host",
    "content-length",
    "proxy-authorization",
    "te",
    "trailer",
    "upgrade",
    "expect",
  };
  for (const auto& name : reserved_names) {
    require_invalid(
      [&] {
        training::http_training_provider provider({
          .base_url = "https://trainer.example",
          .headers = { { name, "unsafe" } },
        });
      },
      "custom HTTP headers must not override protocol, identity, or framing headers");
  }
  require_invalid(
    [] {
      training::http_training_provider provider({
        .base_url = "https://trainer.example",
        .headers = { { "X-Safe\r\nInjected", "value" } },
      });
    },
    "custom HTTP header names must reject CR/LF injection");
  require_invalid(
    [] {
      training::http_training_provider provider({
        .base_url = "https://trainer.example",
        .headers = { { "X-Safe", "value\r\nInjected: true" } },
      });
    },
    "custom HTTP header values must reject CR/LF injection");

  require_invalid(
    [] {
      training::http_training_provider provider({
        .base_url = "http://trainer.example",
      });
    },
    "HTTP training providers require secure transport by default");
  training::http_training_provider explicitly_insecure(
    {
      .base_url = "http://127.0.0.1:8080",
      .allow_insecure_transport = true,
    },
    std::make_shared<recording_http_client>());
  (void)explicitly_insecure;

  require_invalid(
    [] {
      training::http_training_provider provider({
        .base_url = "https://trainer.example",
        .bearer_token = "secret\r\nInjected: true",
      });
    },
    "bearer tokens reject CR/LF injection");
  require_invalid(
    [] {
      training::http_training_provider provider({
        .base_url = "https://trainer.example",
        .headers = { { "Invalid Header", "value" } },
      });
    },
    "custom HTTP header names must use the HTTP token grammar");

  const std::vector<training::training_context> unsafe_contexts {
    { .request_id = "request\r\ninjected" },
    { .trace_id = "trace\nunsafe", .request_id = "safe" },
    { .request_id = "safe", .subject_id = "subject\runsafe" },
    { .request_id = "safe", .tenant_id = "tenant\nunsafe" },
    { .request_id = "safe", .workspace_id = "workspace\runsafe" },
  };
  for (const auto& context : unsafe_contexts) {
    auto http = std::make_shared<recording_http_client>();
    training::http_training_provider provider({ .base_url = "https://trainer.example" }, http);
    require_error(provider.submit(request, context),
      training::training_errc::invalid_request,
      "dynamic HTTP identity values reject CR/LF before network access");
    require(http->requests.empty(), "unsafe HTTP identity values never reach the HTTP client");
  }
  auto unsafe_idempotency_request = request;
  unsafe_idempotency_request.idempotency_key = "key\r\nInjected: true";
  auto unsafe_idempotency_http = std::make_shared<recording_http_client>();
  training::http_training_provider unsafe_idempotency_provider(
    { .base_url = "https://trainer.example" }, unsafe_idempotency_http);
  require_error(
    unsafe_idempotency_provider.submit(unsafe_idempotency_request, { .request_id = "safe" }),
    training::training_errc::invalid_request,
    "HTTP idempotency keys reject CR/LF before network access");
  require(unsafe_idempotency_http->requests.empty(),
    "unsafe HTTP idempotency keys never reach the HTTP client");

  auto direct_http = std::make_shared<recording_http_client>();
  training::http_training_provider direct_provider(
    { .base_url = "https://trainer.example" }, direct_http);
  require_error(direct_provider.inspect("", {}),
    training::training_errc::invalid_request,
    "direct HTTP inspect reports an empty provider job id through training_result");
  require_error(direct_provider.cancel("", {}),
    training::training_errc::invalid_request,
    "direct HTTP cancel reports an empty provider job id through training_result");
  require_error(direct_provider.resume("", {}, {}),
    training::training_errc::invalid_request,
    "direct HTTP resume reports an empty provider job id through training_result");
  auto invalid_direct_request = request;
  invalid_direct_request.base_model.digest.clear();
  require_error(direct_provider.submit(invalid_direct_request, {}),
    training::training_errc::invalid_request,
    "direct HTTP submit validates its typed request before network access");
  require(direct_http->requests.empty(),
    "invalid direct HTTP Provider arguments never reach the transport");

  require_invalid(
    [] {
      training::http_training_provider provider({
        .base_url = "https://trainer.example",
        .bearer_token = std::string("secret\0truncated", 16),
      });
    },
    "bearer tokens reject embedded NUL characters");
  require_invalid(
    [] {
      training::http_training_provider provider({
        .base_url = "https://trainer.example",
        .headers = { { "X-Safe", std::string("value\x7f", 6) } },
      });
    },
    "custom header values reject DEL and other disallowed controls");
  auto control_context_http = std::make_shared<recording_http_client>();
  training::http_training_provider control_context_provider(
    { .base_url = "https://trainer.example" }, control_context_http);
  require_error(
    control_context_provider.submit(request, { .tenant_id = std::string("tenant\0hidden", 13) }),
    training::training_errc::invalid_request,
    "dynamic HTTP identity values reject embedded NUL characters");
  require(control_context_http->requests.empty(),
    "control characters in dynamic identities never reach the HTTP client");
}

void rejects_incomplete_typed_provider_payloads() {
  const std::vector<nlohmann::json> invalid_snapshots {
    { { "provider_job_id", "job" }, { "status", "queued" }, { "progress", 0.0 } },
    { { "provider_job_id", "job" },
      { "status", "queued" },
      { "progress", 0.0 },
      { "current_step", 0 },
      { "total_steps", 0 } },
    { { "provider_job_id", "job" },
      { "status", "running" },
      { "progress", 0.5 },
      { "current_step", 10 },
      { "metrics", { { { "step", 10 }, { "name", "loss" }, { "value", 0.5 } } } } },
    { { "provider_job_id", "job" },
      { "status", "paused" },
      { "progress", 0.5 },
      { "current_step", 10 },
      { "latest_checkpoint",
        { { "id", "checkpoint" },
          { "uri", "s3://checkpoint" },
          { "digest", digest('e') },
          { "step", 10 } } } },
  };
  for (const auto& value : invalid_snapshots) {
    require_invalid([&] { (void)training::training_provider_snapshot_from_json(value); },
      "typed Provider payloads reject missing mandatory fields and invalid zero totals");
  }
}

void file_store_recovers_append_only_job_revisions() {
  const auto directory = std::filesystem::temp_directory_path() /
                         ("wuwe-training-store-" + training::make_training_id("test"));
  std::filesystem::create_directories(directory);
  try {
    const auto request = request_for(sample_dataset().manifest);
    std::string job_id;
    const auto created_at = std::chrono::system_clock::time_point(std::chrono::seconds(123));
    {
      training::file_training_job_store store(directory);
      require(!store.capabilities().durable && !store.capabilities().atomic_mutations &&
                store.capabilities().coordination_scope ==
                  wuwe::agent::core::storage_coordination_scope::process_local,
        "local revision files do not overstate durability or atomicity guarantees");
      auto job = require_success(store.create({
        .id = "local-job-1",
        .provider = "test-provider",
        .tenant_id = "tenant-file",
        .workspace_id = "workspace-file",
        .request = request,
        .provider_state = {
          .provider_job_id = "remote-job-1",
          .status = training::training_job_status::queued,
        },
        .created_at = created_at,
      }), "file store create failed").job;
      job_id = job.id;
      job.provider_state = {
        .provider_job_id = "remote-job-1",
        .status = training::training_job_status::paused,
        .progress = 0.4,
        .current_step = 40,
        .total_steps = 100,
        .metrics = { { .step = 40, .name = "train.loss", .value = 0.5 } },
        .latest_checkpoint =
          training::training_checkpoint {
            .id = "checkpoint-40",
            .uri = "s3://checkpoints/40",
            .digest = digest('e'),
            .step = 40,
          },
      };
      const auto updated =
        require_success(store.update(job, job.revision), "file store update failed");
      require(updated.revision == 2, "file store appends a new immutable revision");
    }
    training::file_training_job_store recovered(directory);
    const auto job = require_success(recovered.load(job_id), "file store load failed");
    require(job && job->revision == 2 &&
              job->provider_state.status == training::training_job_status::paused &&
              job->provider_state.latest_checkpoint &&
              job->provider_state.latest_checkpoint->step == 40 &&
              job->provider_state.metrics.size() == 1 &&
              job->request.base_model == request.base_model && job->created_at == created_at &&
              job->tenant_id == "tenant-file" && job->workspace_id == "workspace-file" &&
              require_success(recovered.find_by_idempotency_key(
                                "tenant-file", "workspace-file", request.idempotency_key),
                "file store scoped lookup failed")
                  ->id == job_id &&
              !require_success(recovered.find_by_idempotency_key(
                                 "other-tenant", "workspace-file", request.idempotency_key),
                "file store scoped lookup failed"),
      "file store reconstructs the latest complete job after restart");
    std::size_t revision_files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
      if (entry.path().extension() == ".json")
        ++revision_files;
    }
    require(revision_files == 2, "file store retains previous revisions for recovery and audit");
  }
  catch (...) {
    std::filesystem::remove_all(directory);
    throw;
  }
  std::filesystem::remove_all(directory);
}

void file_store_rejects_rewritten_revision_history() {
  const auto request = request_for(sample_dataset().manifest);
  const auto write_history = [&](const std::filesystem::path& directory,
                               const std::function<void(training::training_job&)>& rewrite) {
    std::filesystem::create_directories(directory);
    std::string job_id;
    {
      training::file_training_job_store store(directory);
      auto job = require_success(store.create({
        .id = "history-job",
        .provider = "history-provider",
        .request = request,
        .provider_state = {
          .provider_job_id = "history-remote", .status = training::training_job_status::queued,
        },
      }), "history create failed").job;
      job_id = job.id;
      job.provider_state.status = training::training_job_status::running;
      (void)require_success(store.update(job, job.revision), "history update failed");
    }
    const auto revision_path = directory / (wuwe::common::sha256_hex(job_id) + ".2.json");
    nlohmann::json encoded;
    {
      std::ifstream input(revision_path, std::ios::binary);
      require(static_cast<bool>(input), "history revision fixture must be readable");
      input >> encoded;
    }
    auto rewritten = training::training_job_record_from_json(encoded);
    rewrite(rewritten);
    {
      std::ofstream output(revision_path, std::ios::binary | std::ios::trunc);
      require(static_cast<bool>(output), "history revision fixture must be writable");
      output << training::training_job_record_to_json(rewritten).dump(2);
    }
  };
  const auto rejects = [&](std::string suffix,
                         const std::function<void(training::training_job&)>& rewrite,
                         const std::string& message) {
    const auto directory =
      std::filesystem::temp_directory_path() /
      ("wuwe-training-history-" + suffix + "-" + training::make_training_id("test"));
    try {
      write_history(directory, rewrite);
      bool rejected = false;
      try {
        training::file_training_job_store recovered(directory);
      }
      catch (const std::exception&) {
        rejected = true;
      }
      require(rejected, message);
    }
    catch (...) {
      std::filesystem::remove_all(directory);
      throw;
    }
    std::filesystem::remove_all(directory);
  };
  rejects(
    "created",
    [](training::training_job& job) { job.created_at -= std::chrono::hours(1); },
    "file Store replay rejects rewritten creation timestamps");
  rejects(
    "updated",
    [](training::training_job& job) {
      job.updated_at = job.created_at - std::chrono::milliseconds(1);
    },
    "file Store replay rejects update timestamps preceding creation");
  rejects(
    "metadata",
    [](training::training_job& job) { job.metadata["rewritten"] = "true"; },
    "file Store replay rejects rewritten immutable job metadata");
}

void function_provider_has_a_typed_exception_boundary() {
  const auto request = request_for(sample_dataset().manifest);
  training::function_training_provider provider("throwing-provider",
    training::function_training_provider::submit_callback(
      [](const training::training_request&, const training::training_context&)
        -> training::training_result<training::training_provider_snapshot> { throw 7; }),
    training::function_training_provider::inspect_callback(
      [](const std::string&, const training::training_context&)
        -> training::training_result<training::training_provider_snapshot> {
        throw std::runtime_error("");
      }),
    training::function_training_provider::cancel_callback(
      [](const std::string&, const training::training_context&)
        -> training::training_result<training::training_provider_snapshot> {
        throw std::runtime_error("cancel failed");
      }));
  const auto submit = provider.submit(request, {});
  require(submit.error_if() &&
            submit.error_if()->code == training::training_errc::provider_transport &&
            submit.error_if()->remote_outcome == training::training_remote_outcome::uncertain,
    "result callbacks convert unknown submit exceptions into typed uncertain failures");
  const auto inspect = provider.inspect("job", {});
  require(inspect.error_if() &&
            inspect.error_if()->code == training::training_errc::provider_transport &&
            !inspect.error_if()->message.empty(),
    "result callbacks convert empty exception messages into valid typed failures");
  const auto cancel = provider.cancel("job", {});
  require(cancel.error_if() &&
            cancel.error_if()->code == training::training_errc::provider_transport &&
            cancel.error_if()->remote_outcome == training::training_remote_outcome::uncertain,
    "result callbacks convert cancel exceptions into typed uncertain failures");
}

void enforces_training_governance() {
  const auto request = request_for(sample_dataset().manifest);
  std::size_t submissions {};
  auto provider = std::make_shared<training::function_training_provider>(
    "test-provider",
    [&](const training::training_request&, const training::training_context&) {
      ++submissions;
      return training::training_provider_snapshot {
        .provider_job_id = "governed-job",
        .status = training::training_job_status::queued,
      };
    },
    [](const std::string& id, const training::training_context&) {
      return training::training_provider_snapshot {
        .provider_job_id = id,
        .status = training::training_job_status::running,
      };
    },
    [](const std::string& id, const training::training_context&) {
      return training::training_provider_snapshot {
        .provider_job_id = id,
        .status = training::training_job_status::cancelled,
      };
    });

  wuwe::agent::audit::in_memory_audit_sink audit;
  training::in_memory_training_job_store denied_store;
  training::training_coordinator denied(provider,
    denied_store,
    {
      .audit = &audit,
    });
  require_error(denied.submit(request,
                  { .trace_id = "trace-1", .request_id = "request-1", .subject_id = "user-1" }),
    training::training_errc::authorization_denied,
    "approval-gated training must fail closed without an approval service");
  const auto denied_events = audit.events();
  require(submissions == 0 && denied_events.size() == 3 &&
            denied_events.front().outcome == wuwe::agent::audit::audit_event_outcome::allowed &&
            denied_events.back().outcome == wuwe::agent::audit::audit_event_outcome::denied &&
            denied_events.back().attributes.at("stage") == "approval",
    "missing approval records a final denied audit outcome after policy evaluation");

  audit.clear();
  recording_approval_service approvals(wuwe::agent::approval::approval_decision_kind::approved);
  training::in_memory_training_job_store approved_store;
  training::training_coordinator approved(provider,
    approved_store,
    {
      .approvals = &approvals,
      .audit = &audit,
    });
  const auto job =
    require_success(approved.submit(request,
                      { .trace_id = "trace-2", .request_id = "request-2", .subject_id = "user-1" }),
      "approved training submission failed");
  require(job.provider_state.status == training::training_job_status::queued && submissions == 1,
    "approved training reaches the provider exactly once");
  require(approvals.request && approvals.request->metadata.at("tenant_id") == "" &&
            audit.events().back().outcome == wuwe::agent::audit::audit_event_outcome::approved,
    "approval requests and audit evidence carry explicit governance state");

  audit.clear();
  recording_approval_service manual(
    wuwe::agent::approval::approval_decision_kind::needs_manual_review);
  training::in_memory_training_job_store manual_store;
  training::training_coordinator manual_review(provider,
    manual_store,
    {
      .approvals = &manual,
      .audit = &audit,
    });
  require_error(manual_review.submit(request,
                  { .trace_id = "trace-3",
                    .request_id = "request-3",
                    .subject_id = "user-3",
                    .tenant_id = "tenant-3",
                    .workspace_id = "workspace-3" }),
    training::training_errc::authorization_denied,
    "manual review is not equivalent to approval");
  require(manual.request && manual.request->metadata.at("tenant_id") == "tenant-3" &&
            manual.request->metadata.at("workspace_id") == "workspace-3" &&
            audit.events().back().outcome == wuwe::agent::audit::audit_event_outcome::denied &&
            audit.events().back().attributes.at("tenant_id") == "tenant-3" &&
            audit.events().back().attributes.at("workspace_id") == "workspace-3",
    "tenant and workspace context propagate to approval and denied audit evidence");

  audit.clear();
  throwing_approval_service unavailable;
  training::in_memory_training_job_store unavailable_store;
  training::training_coordinator approval_failure(provider,
    unavailable_store,
    {
      .approvals = &unavailable,
      .audit = &audit,
    });
  require_error(approval_failure.submit(request),
    training::training_errc::authorization_denied,
    "approval service exceptions fail closed");
  require(audit.events().back().outcome == wuwe::agent::audit::audit_event_outcome::denied &&
            audit.events().back().attributes.at("decision") == "service_failure",
    "approval service failures produce final denied audit evidence");
}

void rejects_unsupported_serialization_versions() {
  const auto request = request_for(sample_dataset().manifest);
  auto encoded = training::training_request_to_json(request);
  encoded["schema"] = "wuwe.training-request.v2";
  require_invalid([&] { (void)training::training_request_from_json(encoded); },
    "request codecs reject unknown schema versions instead of guessing compatibility");

  encoded = training::training_request_to_json(request);
  encoded.erase("hyperparameters");
  require_invalid([&] { (void)training::training_request_from_json(encoded); },
    "versioned request codecs require the serialized hyperparameter contract");

  encoded = training::training_request_to_json(request);
  encoded["hyperparameters"].erase("seed");
  require_invalid([&] { (void)training::training_request_from_json(encoded); },
    "versioned request codecs do not silently restore missing hyperparameter fields");

  encoded = training::training_request_to_json(request);
  encoded["resources"].erase("maximum_runtime_seconds");
  require_invalid([&] { (void)training::training_request_from_json(encoded); },
    "versioned request codecs do not silently restore missing resource fields");

  encoded = training::training_request_to_json(request);
  encoded["dataset"].erase("created_at_unix_millis");
  require_invalid([&] { (void)training::training_request_from_json(encoded); },
    "versioned request codecs require the serialized dataset timestamp");

  encoded = training::training_request_to_json(request);
  encoded["dataset"].erase("format");
  require_invalid([&] { (void)training::training_request_from_json(encoded); },
    "versioned request codecs do not silently restore the dataset format");

  auto artifact = training::model_artifact_manifest_to_json(artifact_for(request));
  artifact.erase("schema");
  require_invalid([&] { (void)training::model_artifact_manifest_from_json(artifact); },
    "artifact codecs require an explicit supported schema version");

  artifact = training::model_artifact_manifest_to_json(artifact_for(request));
  artifact.erase("hyperparameters");
  require_invalid([&] { (void)training::model_artifact_manifest_from_json(artifact); },
    "versioned artifact codecs require the serialized hyperparameter contract");
}

void run(const char* name, void (*test)()) {
  test();
  (void)name;
}

} // namespace

int main() {
  try {
    run("SFT datasets", builds_reproducible_sft_datasets);
    run("tool trajectories", validates_agent_tool_trajectories);
    run("QLoRA contract", validates_qlora_as_a_real_adaptation_contract);
    run("training resources", validates_training_budget_and_resources);
    run("history time precision", compares_provider_history_at_protocol_time_precision);
    run("strict adaptation JSON", rejects_contradictory_adaptation_json);
    run("submission reconciliation", classifies_submission_failures_and_reconciliation);
    run("ambiguous mutation reconciliation", reconciles_ambiguous_remote_mutation_failures);
    run("submission reconciliation recovery", reconciles_uncertain_submissions_end_to_end);
    run("submission reconciliation identity", validates_reconciliation_submission_identity);
    run("error boundaries and telemetry", validates_error_boundaries_and_best_effort_telemetry);
    run("custom Store boundary", validates_custom_store_success_values_and_provider_identity);
    run("lifecycle reconciliation", reconciles_remote_lifecycle_side_effects);
    run("step and total bounds", validates_step_bounds_and_frozen_total_steps);
    run("training lifecycle", coordinates_idempotent_training_and_model_promotion);
    run("provider validation", rejects_invalid_provider_state_and_artifact_lineage);
    run("checkpoint resume", resumes_only_from_an_attributable_checkpoint);
    run("lifecycle dispatch", dispatches_each_lifecycle_operation_exactly_once);
    run("provider regressions", classifies_resume_exceptions_and_provider_history_regressions);
    run("provider history", rejects_ambiguous_or_rewritten_provider_history);
    run("tenant isolation", isolates_training_jobs_by_tenant_and_workspace);
    run("provider idempotency", rejects_cross_provider_idempotency_reuse);
    run("HTTP provider", http_provider_uses_the_versioned_job_protocol);
    run(
      "HTTP cancellation race", http_provider_preserves_authoritative_responses_after_cancellation);
    run("HTTP lookup and cancellation", http_provider_supports_lookup_and_cooperative_cancellation);
    run("strict HTTP envelopes", rejects_malformed_http_envelopes);
    run("HTTP errors and headers", classifies_http_errors_and_rejects_unsafe_headers);
    run("strict Provider payloads", rejects_incomplete_typed_provider_payloads);
    run("file store recovery", file_store_recovers_append_only_job_revisions);
    run("file store history integrity", file_store_rejects_rewritten_revision_history);
    run("function Provider exception boundary", function_provider_has_a_typed_exception_boundary);
    run("error and revision invariants", rejects_inconsistent_errors_and_revision_overflow);
    run("training governance", enforces_training_governance);
    run("serialization versions", rejects_unsupported_serialization_versions);
  }
  catch (const std::exception& ex) {
    std::cerr << "[FAIL] " << ex.what() << '\n';
    return 1;
  }
  return 0;
}
