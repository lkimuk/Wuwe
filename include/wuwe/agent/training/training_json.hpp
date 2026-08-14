#ifndef WUWE_AGENT_TRAINING_TRAINING_JSON_HPP
#define WUWE_AGENT_TRAINING_TRAINING_JSON_HPP

#include <algorithm>
#include <chrono>
#include <limits>
#include <string>

#include <nlohmann/json.hpp>

#include <wuwe/agent/training/training_core.hpp>
#include <wuwe/common/sha256.hpp>

namespace wuwe::agent::training {

inline nlohmann::json training_tool_call_to_json(const training_tool_call& value) {
  return { { "id", value.id }, { "name", value.name }, { "arguments", value.arguments } };
}

inline nlohmann::json training_tool_definition_to_json(const training_tool_definition& value) {
  return {
    { "name", value.name },
    { "description", value.description },
    { "parameters", value.parameters },
  };
}

inline nlohmann::json training_message_to_json(const training_message& value) {
  auto tool_calls = nlohmann::json::array();
  for (const auto& call : value.tool_calls)
    tool_calls.push_back(training_tool_call_to_json(call));
  return {
    { "role", to_string(value.role) },
    { "content", value.content },
    { "name", value.name },
    { "tool_call_id", value.tool_call_id },
    { "tool_calls", std::move(tool_calls) },
    { "trainable", value.trainable },
    { "metadata", value.metadata },
  };
}

inline nlohmann::json training_example_to_json(const training_example& value) {
  auto tools = nlohmann::json::array();
  for (const auto& tool : value.tools)
    tools.push_back(training_tool_definition_to_json(tool));
  auto messages = nlohmann::json::array();
  for (const auto& message : value.messages)
    messages.push_back(training_message_to_json(message));
  return {
    { "id", value.id },
    { "tools", std::move(tools) },
    { "messages", std::move(messages) },
    { "weight", value.weight },
    { "source", value.source },
    { "source_run_id", value.source_run_id },
    { "metadata", value.metadata },
  };
}

inline std::string canonical_training_dataset_jsonl(const training_dataset& value) {
  std::string output;
  for (const auto& example : value.examples) {
    output += training_example_to_json(example).dump();
    output.push_back('\n');
  }
  return output;
}

inline std::string training_dataset_digest(const training_dataset& value) {
  return common::sha256_hex(canonical_training_dataset_jsonl(value));
}

inline void validate_training_dataset(const training_dataset& value) {
  validate_training_dataset_structure(value);
  if (value.manifest.digest != training_dataset_digest(value)) {
    throw std::invalid_argument(
      "training dataset manifest digest does not match its canonical examples");
  }
}

inline nlohmann::json model_artifact_manifest_to_json(const model_artifact_manifest& value) {
  const auto* lora = lora_config_if(value.adaptation);
  const auto* quantization_config = quantization_config_if(value.adaptation);
  nlohmann::json quantization;
  if (quantization_config) {
    quantization = {
      { "bits", quantization_config->bits },
      { "quantization_type", quantization_config->quantization_type },
      { "compute_dtype", quantization_config->compute_dtype },
      { "double_quantization", quantization_config->double_quantization },
    };
  }
  return {
    { "schema", "wuwe.model-artifact.v1" },
    { "id", value.id },
    { "version", value.version },
    { "objective", to_string(value.objective) },
    { "adaptation", to_string(adaptation_kind(value.adaptation)) },
    { "uri", value.uri },
    { "digest", value.digest },
    { "format", value.format },
    { "base_model",
      {
        { "id", value.base_model.id },
        { "revision", value.base_model.revision },
        { "digest", value.base_model.digest },
        { "tokenizer", value.base_model.tokenizer },
        { "chat_template", value.base_model.chat_template },
        { "metadata", value.base_model.metadata },
      } },
    { "dataset",
      {
        { "id", value.dataset_id },
        { "version", value.dataset_version },
        { "digest", value.dataset_digest },
      } },
    { "tokenizer", value.tokenizer },
    { "chat_template", value.chat_template },
    { "lora", lora ? nlohmann::json {
        { "rank", lora->rank }, { "alpha", lora->alpha }, { "dropout", lora->dropout },
        { "target_modules", lora->target_modules }, { "train_bias", lora->train_bias },
      } : nlohmann::json(nullptr) },
    { "quantization", std::move(quantization) },
    { "hyperparameters",
      {
        { "epochs", value.hyperparameters.epochs },
        { "maximum_steps", value.hyperparameters.maximum_steps },
        { "batch_size", value.hyperparameters.batch_size },
        { "gradient_accumulation_steps", value.hyperparameters.gradient_accumulation_steps },
        { "maximum_sequence_length", value.hyperparameters.maximum_sequence_length },
        { "learning_rate", value.hyperparameters.learning_rate },
        { "warmup_ratio", value.hyperparameters.warmup_ratio },
        { "seed", value.hyperparameters.seed },
        { "numeric", value.hyperparameters.numeric },
        { "options", value.hyperparameters.options },
      } },
    { "provider", value.provider },
    { "provider_job_id", value.provider_job_id },
    { "created_at_unix_millis",
      std::chrono::duration_cast<std::chrono::milliseconds>(value.created_at.time_since_epoch())
        .count() },
    { "metadata", value.metadata },
  };
}

inline nlohmann::json training_request_to_json(const training_request& value) {
  const auto* lora = lora_config_if(value.adaptation);
  const auto* quantization_config = quantization_config_if(value.adaptation);
  nlohmann::json quantization;
  if (quantization_config) {
    quantization = {
      { "bits", quantization_config->bits },
      { "quantization_type", quantization_config->quantization_type },
      { "compute_dtype", quantization_config->compute_dtype },
      { "double_quantization", quantization_config->double_quantization },
    };
  }
  return {
    { "schema", "wuwe.training-request.v1" },
    { "id", value.id },
    { "idempotency_key", value.idempotency_key },
    { "target", value.target },
    { "objective", to_string(value.objective) },
    { "adaptation", to_string(adaptation_kind(value.adaptation)) },
    { "base_model",
      {
        { "id", value.base_model.id },
        { "revision", value.base_model.revision },
        { "digest", value.base_model.digest },
        { "tokenizer", value.base_model.tokenizer },
        { "chat_template", value.base_model.chat_template },
        { "metadata", value.base_model.metadata },
      } },
    { "dataset",
      {
        { "id", value.dataset.id },
        { "version", value.dataset.version },
        { "format", value.dataset.format },
        { "uri", value.dataset.uri },
        { "digest", value.dataset.digest },
        { "example_count", value.dataset.example_count },
        { "message_count", value.dataset.message_count },
        { "tokenizer", value.dataset.tokenizer },
        { "chat_template", value.dataset.chat_template },
        { "created_at_unix_millis",
          std::chrono::duration_cast<std::chrono::milliseconds>(
            value.dataset.created_at.time_since_epoch())
            .count() },
        { "metadata", value.dataset.metadata },
      } },
    { "lora", lora ? nlohmann::json {
        { "rank", lora->rank }, { "alpha", lora->alpha }, { "dropout", lora->dropout },
        { "target_modules", lora->target_modules }, { "train_bias", lora->train_bias },
      } : nlohmann::json(nullptr) },
    { "quantization", std::move(quantization) },
    { "hyperparameters",
      {
        { "epochs", value.hyperparameters.epochs },
        { "maximum_steps", value.hyperparameters.maximum_steps },
        { "batch_size", value.hyperparameters.batch_size },
        { "gradient_accumulation_steps", value.hyperparameters.gradient_accumulation_steps },
        { "maximum_sequence_length", value.hyperparameters.maximum_sequence_length },
        { "learning_rate", value.hyperparameters.learning_rate },
        { "warmup_ratio", value.hyperparameters.warmup_ratio },
        { "seed", value.hyperparameters.seed },
        { "numeric", value.hyperparameters.numeric },
        { "options", value.hyperparameters.options },
      } },
    { "resources",
      {
        { "accelerators", value.resources.accelerators },
        { "accelerator_type", value.resources.accelerator_type },
        { "memory_gib", value.resources.memory_gib },
        { "maximum_runtime_seconds", value.resources.maximum_runtime.count() },
        { "queue", value.resources.queue },
        { "metadata", value.resources.metadata },
      } },
    { "output_uri", value.output_uri },
    { "metadata", value.metadata },
  };
}

inline std::string training_request_digest(const training_request& value) {
  auto semantic = training_request_to_json(value);
  semantic.erase("idempotency_key");
  semantic["dataset"].erase("created_at_unix_millis");
  return common::sha256_hex(semantic.dump());
}

inline void validate_training_job(const training_job& value) {
  if (value.id.empty() || value.provider.empty())
    throw std::invalid_argument("training job requires id and provider");
  validate_training_request(value.request);
  validate_provider_snapshot(value.provider_state);
  if (value.provider_state.artifact) {
    validate_model_artifact(*value.provider_state.artifact, &value.request);
    if (value.provider_state.artifact->provider != value.provider ||
        value.provider_state.artifact->provider_job_id != value.provider_state.provider_job_id) {
      throw std::invalid_argument("training job artifact origin does not match its provider");
    }
  }
  if (value.updated_at < value.created_at)
    throw std::invalid_argument("training job updated timestamp precedes its creation timestamp");
}

inline void validate_training_job_update(const training_job& previous, const training_job& next) {
  validate_training_job(previous);
  validate_training_job(next);
  if (next.id != previous.id || next.provider != previous.provider ||
      next.tenant_id != previous.tenant_id || next.workspace_id != previous.workspace_id ||
      training_request_to_json(next.request) != training_request_to_json(previous.request) ||
      next.metadata != previous.metadata) {
    throw std::invalid_argument("training job identity and request are immutable");
  }
  if (!same_training_time(next.created_at, previous.created_at))
    throw std::invalid_argument("training job creation timestamp is immutable");
  if (next.updated_at < previous.updated_at)
    throw std::invalid_argument("training job updated timestamp must not move backwards");
  if (next.revision != previous.revision &&
      (previous.revision == (std::numeric_limits<std::uint64_t>::max)() ||
        next.revision != previous.revision + 1))
    throw std::invalid_argument(
      "training job revision must remain current or advance exactly once");
  if (next.provider_state.provider_job_id != previous.provider_state.provider_job_id)
    throw std::invalid_argument("training provider job id is immutable");
  if (!valid_transition(previous.provider_state.status, next.provider_state.status)) {
    throw std::invalid_argument("invalid training job status transition from " +
                                to_string(previous.provider_state.status) + " to " +
                                to_string(next.provider_state.status));
  }
  if (next.provider_state.progress < previous.provider_state.progress)
    throw std::invalid_argument("training progress must not move backwards");
  if (next.provider_state.current_step < previous.provider_state.current_step)
    throw std::invalid_argument("training current step must not move backwards");
  if (previous.provider_state.total_steps &&
      next.provider_state.total_steps != previous.provider_state.total_steps) {
    throw std::invalid_argument("training total steps are immutable once declared");
  }
  if (previous.provider_state.latest_checkpoint && !next.provider_state.latest_checkpoint)
    throw std::invalid_argument("training checkpoint must not disappear from job history");
  if (next.provider_state.latest_checkpoint && previous.provider_state.latest_checkpoint &&
      next.provider_state.latest_checkpoint->step <
        previous.provider_state.latest_checkpoint->step) {
    throw std::invalid_argument("training checkpoint step must not move backwards");
  }
  if (next.provider_state.latest_checkpoint && previous.provider_state.latest_checkpoint &&
      next.provider_state.latest_checkpoint->step ==
        previous.provider_state.latest_checkpoint->step &&
      !same_training_checkpoint(
        *next.provider_state.latest_checkpoint, *previous.provider_state.latest_checkpoint)) {
    throw std::invalid_argument("training checkpoint history is immutable");
  }
  for (const auto& metric : previous.provider_state.metrics) {
    const auto found = std::find_if(next.provider_state.metrics.begin(),
      next.provider_state.metrics.end(),
      [&](const auto& candidate) {
        return candidate.step == metric.step && candidate.name == metric.name;
      });
    if (found == next.provider_state.metrics.end())
      throw std::invalid_argument("training metric history must not discard observations");
    if (!same_training_metric(*found, metric))
      throw std::invalid_argument("training metric history is immutable");
  }
}

inline nlohmann::json training_job_to_json(const training_job& value) {
  auto metrics = nlohmann::json::array();
  for (const auto& metric : value.provider_state.metrics) {
    metrics.push_back({
      { "step", metric.step },
      { "name", metric.name },
      { "value", metric.value },
      { "metadata", metric.metadata },
    });
  }
  nlohmann::json checkpoint;
  if (value.provider_state.latest_checkpoint) {
    const auto& item = *value.provider_state.latest_checkpoint;
    checkpoint = {
      { "id", item.id },
      { "uri", item.uri },
      { "digest", item.digest },
      { "step", item.step },
      { "metadata", item.metadata },
    };
  }
  nlohmann::json artifact;
  if (value.provider_state.artifact)
    artifact = model_artifact_manifest_to_json(*value.provider_state.artifact);
  return {
    { "id", value.id },
    { "provider", value.provider },
    { "tenant_id", value.tenant_id },
    { "workspace_id", value.workspace_id },
    { "revision", value.revision },
    { "request_id", value.request.id },
    { "idempotency_key", value.request.idempotency_key },
    { "target", value.request.target },
    { "objective", to_string(value.request.objective) },
    { "adaptation", to_string(adaptation_kind(value.request.adaptation)) },
    { "provider_job_id", value.provider_state.provider_job_id },
    { "status", to_string(value.provider_state.status) },
    { "progress", value.provider_state.progress },
    { "current_step", value.provider_state.current_step },
    { "total_steps", value.provider_state.total_steps },
    { "status_message", value.provider_state.status_message },
    { "error_code", value.provider_state.error_code },
    { "error", value.provider_state.error },
    { "metrics", std::move(metrics) },
    { "latest_checkpoint", std::move(checkpoint) },
    { "artifact", std::move(artifact) },
    { "metadata", value.metadata },
  };
}

} // namespace wuwe::agent::training

#endif // WUWE_AGENT_TRAINING_TRAINING_JSON_HPP
