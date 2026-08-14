#ifndef WUWE_AGENT_TRAINING_TRAINING_CORE_HPP
#define WUWE_AGENT_TRAINING_TRAINING_CORE_HPP

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace wuwe::agent::training {

enum class training_objective {
  supervised_fine_tuning,
};

enum class training_adaptation {
  full,
  lora,
  qlora,
};

enum class training_job_status {
  submitted,
  queued,
  running,
  paused,
  succeeded,
  failed,
  cancelled,
};

enum class training_message_role {
  system,
  user,
  assistant,
  tool,
};

[[nodiscard]] inline std::string to_string(training_objective value) {
  switch (value) {
    case training_objective::supervised_fine_tuning:
      return "supervised_fine_tuning";
  }
  return "unknown";
}

[[nodiscard]] inline std::string to_string(training_adaptation value) {
  switch (value) {
    case training_adaptation::full:
      return "full";
    case training_adaptation::lora:
      return "lora";
    case training_adaptation::qlora:
      return "qlora";
  }
  return "unknown";
}

[[nodiscard]] inline std::string to_string(training_job_status value) {
  switch (value) {
    case training_job_status::submitted:
      return "submitted";
    case training_job_status::queued:
      return "queued";
    case training_job_status::running:
      return "running";
    case training_job_status::paused:
      return "paused";
    case training_job_status::succeeded:
      return "succeeded";
    case training_job_status::failed:
      return "failed";
    case training_job_status::cancelled:
      return "cancelled";
  }
  return "unknown";
}

[[nodiscard]] inline std::string to_string(training_message_role value) {
  switch (value) {
    case training_message_role::system:
      return "system";
    case training_message_role::user:
      return "user";
    case training_message_role::assistant:
      return "assistant";
    case training_message_role::tool:
      return "tool";
  }
  return "unknown";
}

[[nodiscard]] inline bool terminal(training_job_status value) noexcept {
  return value == training_job_status::succeeded || value == training_job_status::failed ||
         value == training_job_status::cancelled;
}

[[nodiscard]] inline bool valid_transition(
  training_job_status from, training_job_status to) noexcept {
  if (from == to)
    return !terminal(from);
  if (terminal(from))
    return false;
  switch (from) {
    case training_job_status::submitted:
      return to == training_job_status::queued || to == training_job_status::running ||
             to == training_job_status::paused || to == training_job_status::succeeded ||
             to == training_job_status::failed || to == training_job_status::cancelled;
    case training_job_status::queued:
      return to == training_job_status::running || to == training_job_status::paused ||
             to == training_job_status::succeeded || to == training_job_status::failed ||
             to == training_job_status::cancelled;
    case training_job_status::running:
      return to == training_job_status::paused || to == training_job_status::succeeded ||
             to == training_job_status::failed || to == training_job_status::cancelled;
    case training_job_status::paused:
      return to == training_job_status::queued || to == training_job_status::running ||
             to == training_job_status::succeeded || to == training_job_status::failed ||
             to == training_job_status::cancelled;
    case training_job_status::succeeded:
    case training_job_status::failed:
    case training_job_status::cancelled:
      return false;
  }
  return false;
}

struct training_tool_call {
  std::string id;
  std::string name;
  nlohmann::json arguments = nlohmann::json::object();
};

struct training_tool_definition {
  std::string name;
  std::string description;
  nlohmann::json parameters = nlohmann::json::object();
};

struct training_message {
  training_message_role role { training_message_role::user };
  std::string content;
  std::string name;
  std::string tool_call_id;
  std::vector<training_tool_call> tool_calls;
  bool trainable { false };
  std::map<std::string, std::string> metadata;
};

struct training_example {
  std::string id;
  std::vector<training_tool_definition> tools;
  std::vector<training_message> messages;
  double weight { 1.0 };
  std::string source;
  std::string source_run_id;
  std::map<std::string, std::string> metadata;
};

struct training_dataset_manifest {
  std::string id;
  std::string version;
  std::string format { "wuwe.agent-messages.v1" };
  std::string uri;
  std::string digest;
  std::size_t example_count {};
  std::size_t message_count {};
  std::string tokenizer;
  std::string chat_template;
  std::chrono::system_clock::time_point created_at { std::chrono::system_clock::now() };
  std::map<std::string, std::string> metadata;
};

struct training_dataset {
  training_dataset_manifest manifest;
  std::vector<training_example> examples;
};

struct model_reference {
  std::string id;
  std::string revision;
  std::string digest;
  std::string tokenizer;
  std::string chat_template;
  std::map<std::string, std::string> metadata;

  bool operator==(const model_reference&) const = default;
};

struct lora_configuration {
  std::size_t rank { 16 };
  double alpha { 32.0 };
  double dropout { 0.0 };
  std::vector<std::string> target_modules;
  bool train_bias { false };

  bool operator==(const lora_configuration&) const = default;
};

struct quantization_configuration {
  std::size_t bits { 4 };
  std::string quantization_type { "nf4" };
  std::string compute_dtype { "bfloat16" };
  bool double_quantization { true };

  bool operator==(const quantization_configuration&) const = default;
};

struct full_training_config {
  bool operator==(const full_training_config&) const = default;
};

struct lora_training_config {
  lora_configuration lora;
  bool operator==(const lora_training_config&) const = default;
};

struct qlora_training_config {
  lora_configuration lora;
  quantization_configuration quantization;
  bool operator==(const qlora_training_config&) const = default;
};

using adaptation_config =
  std::variant<full_training_config, lora_training_config, qlora_training_config>;

[[nodiscard]] inline training_adaptation adaptation_kind(const adaptation_config& value) noexcept {
  if (std::holds_alternative<full_training_config>(value))
    return training_adaptation::full;
  if (std::holds_alternative<lora_training_config>(value))
    return training_adaptation::lora;
  return training_adaptation::qlora;
}

[[nodiscard]] inline const lora_configuration* lora_config_if(
  const adaptation_config& value) noexcept {
  if (const auto* config = std::get_if<lora_training_config>(&value))
    return &config->lora;
  if (const auto* config = std::get_if<qlora_training_config>(&value))
    return &config->lora;
  return nullptr;
}

[[nodiscard]] inline const quantization_configuration* quantization_config_if(
  const adaptation_config& value) noexcept {
  const auto* config = std::get_if<qlora_training_config>(&value);
  return config ? &config->quantization : nullptr;
}

struct training_hyperparameters {
  std::size_t epochs { 1 };
  std::size_t maximum_steps {};
  std::size_t batch_size { 1 };
  std::size_t gradient_accumulation_steps { 1 };
  std::size_t maximum_sequence_length { 2048 };
  double learning_rate { 2e-4 };
  double warmup_ratio { 0.0 };
  std::uint64_t seed { 42 };
  std::map<std::string, double> numeric;
  std::map<std::string, std::string> options;

  bool operator==(const training_hyperparameters&) const = default;
};

struct training_resource_request {
  std::size_t accelerators { 1 };
  std::string accelerator_type;
  std::size_t memory_gib {};
  std::chrono::seconds maximum_runtime { 0 };
  std::string queue;
  std::map<std::string, std::string> metadata;
};

struct training_request {
  std::string id;
  std::string idempotency_key;
  std::string target;
  training_objective objective { training_objective::supervised_fine_tuning };
  adaptation_config adaptation { lora_training_config {} };
  model_reference base_model;
  training_dataset_manifest dataset;
  training_hyperparameters hyperparameters;
  training_resource_request resources;
  std::string output_uri;
  std::map<std::string, std::string> metadata;
};

struct training_metric {
  std::size_t step {};
  std::string name;
  double value {};
  std::chrono::system_clock::time_point observed_at { std::chrono::system_clock::now() };
  std::map<std::string, std::string> metadata;

  bool operator==(const training_metric&) const = default;
};

struct training_checkpoint {
  std::string id;
  std::string uri;
  std::string digest;
  std::size_t step {};
  std::chrono::system_clock::time_point created_at { std::chrono::system_clock::now() };
  std::map<std::string, std::string> metadata;

  bool operator==(const training_checkpoint&) const = default;
};

[[nodiscard]] inline bool same_training_time(
  std::chrono::system_clock::time_point lhs, std::chrono::system_clock::time_point rhs) noexcept {
  return std::chrono::duration_cast<std::chrono::milliseconds>(lhs.time_since_epoch()) ==
         std::chrono::duration_cast<std::chrono::milliseconds>(rhs.time_since_epoch());
}

[[nodiscard]] inline bool same_training_metric(
  const training_metric& lhs, const training_metric& rhs) noexcept {
  return lhs.step == rhs.step && lhs.name == rhs.name && lhs.value == rhs.value &&
         same_training_time(lhs.observed_at, rhs.observed_at) && lhs.metadata == rhs.metadata;
}

[[nodiscard]] inline bool same_training_checkpoint(
  const training_checkpoint& lhs, const training_checkpoint& rhs) noexcept {
  return lhs.id == rhs.id && lhs.uri == rhs.uri && lhs.digest == rhs.digest &&
         lhs.step == rhs.step && same_training_time(lhs.created_at, rhs.created_at) &&
         lhs.metadata == rhs.metadata;
}

struct model_artifact_manifest {
  std::string id;
  std::string version;
  training_objective objective { training_objective::supervised_fine_tuning };
  adaptation_config adaptation { lora_training_config {} };
  std::string uri;
  std::string digest;
  std::string format;
  model_reference base_model;
  std::string dataset_id;
  std::string dataset_version;
  std::string dataset_digest;
  std::string tokenizer;
  std::string chat_template;
  training_hyperparameters hyperparameters;
  std::string provider;
  std::string provider_job_id;
  std::chrono::system_clock::time_point created_at { std::chrono::system_clock::now() };
  std::map<std::string, std::string> metadata;
};

struct training_provider_snapshot {
  std::string provider_job_id;
  training_job_status status { training_job_status::submitted };
  double progress {};
  std::size_t current_step {};
  std::optional<std::size_t> total_steps;
  std::string status_message;
  std::string error_code;
  std::string error;
  std::vector<training_metric> metrics;
  std::optional<training_checkpoint> latest_checkpoint;
  std::optional<model_artifact_manifest> artifact;
  std::map<std::string, std::string> metadata;
};

struct training_submission_record {
  std::string idempotency_key;
  std::string request_id;
  std::string request_digest;
  training_provider_snapshot snapshot;
};

struct training_job {
  std::string id;
  std::string provider;
  std::string tenant_id;
  std::string workspace_id;
  std::uint64_t revision {};
  training_request request;
  training_provider_snapshot provider_state;
  std::chrono::system_clock::time_point created_at { std::chrono::system_clock::now() };
  std::chrono::system_clock::time_point updated_at { std::chrono::system_clock::now() };
  std::map<std::string, std::string> metadata;
};

inline std::string make_training_id(const char* prefix) {
  static std::atomic<std::uint64_t> next { 1 };
  const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
    std::chrono::system_clock::now().time_since_epoch())
                     .count();
  return std::string(prefix) + "-" + std::to_string(now) + "-" +
         std::to_string(next.fetch_add(1, std::memory_order_relaxed));
}

[[nodiscard]] inline bool canonical_sha256(const std::string& value) noexcept {
  return value.size() == 64 && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
  });
}

inline void validate_training_example(const training_example& value) {
  if (value.id.empty())
    throw std::invalid_argument("training example id must not be empty");
  if (value.messages.empty())
    throw std::invalid_argument("training example messages must not be empty");
  if (!std::isfinite(value.weight) || value.weight <= 0.0)
    throw std::invalid_argument("training example weight must be finite and greater than zero");

  std::set<std::string> pending_tool_calls;
  std::set<std::string> seen_tool_call_ids;
  std::set<std::string> tools;
  for (const auto& tool : value.tools) {
    if (tool.name.empty() || !tool.parameters.is_object())
      throw std::invalid_argument("training tools require a name and object parameter schema");
    if (!tools.insert(tool.name).second)
      throw std::invalid_argument("training tool names must be unique within an example");
  }
  bool trainable = false;
  for (const auto& message : value.messages) {
    if (!pending_tool_calls.empty() && message.role != training_message_role::tool) {
      throw std::invalid_argument(
        "tool results must immediately follow their assistant tool calls");
    }
    trainable = trainable || message.trainable;
    if (message.role != training_message_role::assistant && !message.tool_calls.empty()) {
      throw std::invalid_argument("only assistant training messages may contain tool calls");
    }
    if (message.role == training_message_role::tool) {
      if (message.tool_call_id.empty() || pending_tool_calls.erase(message.tool_call_id) == 0) {
        throw std::invalid_argument("tool training message must match a preceding tool call");
      }
    }
    else if (!message.tool_call_id.empty()) {
      throw std::invalid_argument("tool_call_id is only valid on tool training messages");
    }
    for (const auto& call : message.tool_calls) {
      if (call.id.empty() || call.name.empty() || !call.arguments.is_object()) {
        throw std::invalid_argument("training tool calls require id, name, and object arguments");
      }
      if (!seen_tool_call_ids.insert(call.id).second) {
        throw std::invalid_argument("training tool call ids must be unique within an example");
      }
      pending_tool_calls.insert(call.id);
      if (!tools.contains(call.name))
        throw std::invalid_argument("training tool calls must reference a declared tool schema");
    }
    if (message.content.empty() && message.tool_calls.empty())
      throw std::invalid_argument("training messages require content or assistant tool calls");
    if (message.role != training_message_role::assistant && message.trainable) {
      throw std::invalid_argument("only assistant training messages may be trainable");
    }
  }
  if (!pending_tool_calls.empty())
    throw std::invalid_argument("every training tool call must have a matching tool result");
  if (!trainable)
    throw std::invalid_argument(
      "training example requires at least one trainable assistant message");
}

inline void validate_training_dataset_structure(const training_dataset& value) {
  if (value.manifest.id.empty() || value.manifest.version.empty())
    throw std::invalid_argument("training dataset requires id and version");
  if (value.manifest.format.empty())
    throw std::invalid_argument("training dataset format must not be empty");
  if (!canonical_sha256(value.manifest.digest))
    throw std::invalid_argument("training dataset requires a canonical SHA-256 digest");
  if (value.examples.empty())
    throw std::invalid_argument("training dataset must not be empty");
  std::set<std::string> ids;
  std::size_t message_count = 0;
  for (const auto& example : value.examples) {
    validate_training_example(example);
    if (!ids.insert(example.id).second)
      throw std::invalid_argument("training dataset example ids must be unique");
    message_count += example.messages.size();
  }
  if (value.manifest.example_count != value.examples.size() ||
      value.manifest.message_count != message_count) {
    throw std::invalid_argument("training dataset manifest counts do not match its examples");
  }
}

inline void validate_adaptation_configuration(const adaptation_config& adaptation);

inline void validate_training_request(const training_request& value) {
  if (value.id.empty() || value.idempotency_key.empty() || value.target.empty())
    throw std::invalid_argument("training request requires id, idempotency key, and target");
  if (value.base_model.id.empty() || value.base_model.revision.empty() ||
      !canonical_sha256(value.base_model.digest) || value.base_model.tokenizer.empty() ||
      value.base_model.chat_template.empty()) {
    throw std::invalid_argument(
      "training request base model requires id, revision, digest, tokenizer, and chat template");
  }
  if (value.dataset.id.empty() || value.dataset.version.empty() || value.dataset.uri.empty() ||
      value.dataset.format.empty() || !canonical_sha256(value.dataset.digest) ||
      value.dataset.example_count == 0 || value.dataset.message_count == 0) {
    throw std::invalid_argument("training request requires a versioned dataset digest");
  }
  if (value.dataset.tokenizer != value.base_model.tokenizer ||
      value.dataset.chat_template != value.base_model.chat_template) {
    throw std::invalid_argument(
      "training dataset tokenizer and chat template must match the base model");
  }
  if (value.output_uri.empty())
    throw std::invalid_argument("training request output URI must not be empty");
  if ((value.hyperparameters.epochs == 0) == (value.hyperparameters.maximum_steps == 0) ||
      value.hyperparameters.batch_size == 0 ||
      value.hyperparameters.gradient_accumulation_steps == 0 ||
      value.hyperparameters.maximum_sequence_length == 0 ||
      !std::isfinite(value.hyperparameters.learning_rate) ||
      value.hyperparameters.learning_rate <= 0.0 ||
      !std::isfinite(value.hyperparameters.warmup_ratio) ||
      value.hyperparameters.warmup_ratio < 0.0 || value.hyperparameters.warmup_ratio > 1.0) {
    throw std::invalid_argument("training request contains invalid hyperparameters");
  }
  for (const auto& [_, numeric] : value.hyperparameters.numeric) {
    if (!std::isfinite(numeric))
      throw std::invalid_argument("training numeric hyperparameters must be finite");
  }
  if (value.resources.accelerators == 0 || value.resources.accelerator_type.empty() ||
      value.resources.memory_gib == 0 || value.resources.maximum_runtime.count() < 0) {
    throw std::invalid_argument("training resources require accelerators, accelerator type, "
                                "memory, and non-negative runtime");
  }
  validate_adaptation_configuration(value.adaptation);
}

inline void validate_adaptation_configuration(const adaptation_config& adaptation) {
  if (const auto* lora = lora_config_if(adaptation)) {
    if (lora->rank == 0 || !std::isfinite(lora->alpha) || lora->alpha <= 0.0 ||
        !std::isfinite(lora->dropout) || lora->dropout < 0.0 || lora->dropout >= 1.0 ||
        lora->target_modules.empty()) {
      throw std::invalid_argument(
        "LoRA adaptation requires valid rank, alpha, dropout, and targets");
    }
  }
  if (const auto* quantization = quantization_config_if(adaptation)) {
    if ((quantization->bits != 4 && quantization->bits != 8) ||
        quantization->quantization_type.empty() || quantization->compute_dtype.empty()) {
      throw std::invalid_argument("QLoRA adaptation requires a valid quantization configuration");
    }
  }
}

inline void validate_model_artifact(
  const model_artifact_manifest& value, const training_request* request = nullptr) {
  if (value.id.empty() || value.version.empty() || value.uri.empty() ||
      !canonical_sha256(value.digest) || value.format.empty() || value.base_model.id.empty() ||
      value.base_model.revision.empty() || value.base_model.tokenizer.empty() ||
      value.base_model.chat_template.empty() || value.dataset_id.empty() ||
      value.dataset_version.empty() || value.tokenizer.empty() || value.chat_template.empty() ||
      !canonical_sha256(value.dataset_digest) || !canonical_sha256(value.base_model.digest)) {
    throw std::invalid_argument("model artifact manifest is incomplete");
  }
  validate_adaptation_configuration(value.adaptation);
  if (!request)
    return;
  if (value.objective != request->objective || value.adaptation != request->adaptation ||
      value.base_model.id != request->base_model.id ||
      value.base_model.revision != request->base_model.revision ||
      value.base_model.digest != request->base_model.digest ||
      value.base_model.tokenizer != request->base_model.tokenizer ||
      value.base_model.chat_template != request->base_model.chat_template ||
      value.dataset_id != request->dataset.id ||
      value.dataset_version != request->dataset.version ||
      value.dataset_digest != request->dataset.digest ||
      value.tokenizer != request->base_model.tokenizer ||
      value.chat_template != request->base_model.chat_template ||
      value.hyperparameters != request->hyperparameters) {
    throw std::invalid_argument("model artifact lineage does not match its training request");
  }
}

inline void validate_training_checkpoint(const training_checkpoint& value) {
  if (value.id.empty() || value.uri.empty() || !canonical_sha256(value.digest))
    throw std::invalid_argument("training checkpoint is incomplete");
}

inline void validate_provider_snapshot(const training_provider_snapshot& value) {
  if (value.provider_job_id.empty())
    throw std::invalid_argument("training provider snapshot requires a provider job id");
  if (!std::isfinite(value.progress) || value.progress < 0.0 || value.progress > 1.0)
    throw std::invalid_argument("training progress must be finite and within [0, 1]");
  if (value.total_steps) {
    if (*value.total_steps == 0)
      throw std::invalid_argument("training total steps must be greater than zero");
    if (value.current_step > *value.total_steps)
      throw std::invalid_argument("training current step exceeds total steps");
  }
  std::set<std::pair<std::size_t, std::string>> metric_keys;
  for (const auto& metric : value.metrics) {
    if (metric.name.empty() || !std::isfinite(metric.value))
      throw std::invalid_argument("training metrics require a name and finite value");
    if (metric.step > value.current_step)
      throw std::invalid_argument("training metric step must not exceed current step");
    if (!metric_keys.emplace(metric.step, metric.name).second)
      throw std::invalid_argument("training metrics require unique step and name pairs");
  }
  if (value.latest_checkpoint) {
    validate_training_checkpoint(*value.latest_checkpoint);
    if (value.latest_checkpoint->step > value.current_step)
      throw std::invalid_argument("training checkpoint is ahead of job progress");
  }
  if (value.status == training_job_status::succeeded) {
    if (!value.artifact)
      throw std::invalid_argument("successful training snapshot requires a model artifact");
    if (value.progress != 1.0)
      throw std::invalid_argument("successful training snapshot requires progress equal to one");
  }
  else if (value.artifact) {
    throw std::invalid_argument("only successful training snapshots may publish a model artifact");
  }
  if (value.status == training_job_status::failed && value.error.empty())
    throw std::invalid_argument("failed training snapshot requires an error");
}

inline void validate_training_submission_record(const training_submission_record& value) {
  if (value.idempotency_key.empty() || value.request_id.empty() ||
      !canonical_sha256(value.request_digest)) {
    throw std::invalid_argument(
      "training submission record requires idempotency key, request id, and request digest");
  }
  validate_provider_snapshot(value.snapshot);
}

} // namespace wuwe::agent::training

#endif // WUWE_AGENT_TRAINING_TRAINING_CORE_HPP
