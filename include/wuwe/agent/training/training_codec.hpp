#ifndef WUWE_AGENT_TRAINING_TRAINING_CODEC_HPP
#define WUWE_AGENT_TRAINING_TRAINING_CODEC_HPP

#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include <wuwe/agent/training/training_json.hpp>

namespace wuwe::agent::training {

inline void require_json_field(const nlohmann::json& value, const char* key,
  nlohmann::json::value_t type, const char* object_name) {
  if (!value.contains(key) || value.at(key).type() != type)
    throw std::invalid_argument(std::string(object_name) + " requires " + key);
}

inline void require_json_number_field(
  const nlohmann::json& value, const char* key, const char* object_name) {
  if (!value.contains(key) || !value.at(key).is_number())
    throw std::invalid_argument(std::string(object_name) + " requires numeric " + key);
}

inline void require_json_integer_field(
  const nlohmann::json& value, const char* key, const char* object_name) {
  if (!value.contains(key) || !value.at(key).is_number_integer())
    throw std::invalid_argument(std::string(object_name) + " requires integer " + key);
}

inline std::chrono::system_clock::time_point training_time_from_json(
  const nlohmann::json& value, const char* key) {
  return std::chrono::system_clock::time_point(
    std::chrono::milliseconds(value.value(key, std::int64_t {})));
}

inline training_job_status training_job_status_from_string(const std::string& value) {
  if (value == "submitted")
    return training_job_status::submitted;
  if (value == "queued")
    return training_job_status::queued;
  if (value == "running")
    return training_job_status::running;
  if (value == "paused")
    return training_job_status::paused;
  if (value == "succeeded")
    return training_job_status::succeeded;
  if (value == "failed")
    return training_job_status::failed;
  if (value == "cancelled")
    return training_job_status::cancelled;
  throw std::invalid_argument("unknown training job status: " + value);
}

inline training_objective training_objective_from_string(const std::string& value) {
  if (value == "supervised_fine_tuning")
    return training_objective::supervised_fine_tuning;
  throw std::invalid_argument("unknown training objective: " + value);
}

inline training_adaptation training_adaptation_from_string(const std::string& value) {
  if (value == "full")
    return training_adaptation::full;
  if (value == "lora")
    return training_adaptation::lora;
  if (value == "qlora")
    return training_adaptation::qlora;
  throw std::invalid_argument("unknown training adaptation: " + value);
}

inline model_reference model_reference_from_json(const nlohmann::json& value) {
  if (!value.is_object())
    throw std::invalid_argument("model reference must be a JSON object");
  return {
    .id = value.value("id", std::string {}),
    .revision = value.value("revision", std::string {}),
    .digest = value.value("digest", std::string {}),
    .tokenizer = value.value("tokenizer", std::string {}),
    .chat_template = value.value("chat_template", std::string {}),
    .metadata = value.value("metadata", std::map<std::string, std::string> {}),
  };
}

inline lora_configuration lora_configuration_from_json(const nlohmann::json& value) {
  if (!value.is_object())
    throw std::invalid_argument("LoRA configuration must be a JSON object");
  if (!value.contains("rank") || !value.contains("alpha") || !value.contains("dropout") ||
      !value.contains("target_modules") || !value.contains("train_bias")) {
    throw std::invalid_argument("LoRA configuration requires all contract fields");
  }
  return {
    .rank = value.value("rank", std::size_t { 16 }),
    .alpha = value.value("alpha", 32.0),
    .dropout = value.value("dropout", 0.0),
    .target_modules = value.value("target_modules", std::vector<std::string> {}),
    .train_bias = value.value("train_bias", false),
  };
}

inline quantization_configuration quantization_configuration_from_json(
  const nlohmann::json& value) {
  if (!value.is_object())
    throw std::invalid_argument("quantization configuration must be a JSON object");
  if (!value.contains("bits") || !value.contains("quantization_type") ||
      !value.contains("compute_dtype") || !value.contains("double_quantization")) {
    throw std::invalid_argument("quantization configuration requires all contract fields");
  }
  return {
    .bits = value.value("bits", std::size_t { 4 }),
    .quantization_type = value.value("quantization_type", std::string {}),
    .compute_dtype = value.value("compute_dtype", std::string {}),
    .double_quantization = value.value("double_quantization", true),
  };
}

inline adaptation_config adaptation_configuration_from_json(const nlohmann::json& value) {
  if (!value.is_object() || !value.contains("adaptation") || !value.at("adaptation").is_string()) {
    throw std::invalid_argument("adaptation configuration requires a string discriminator");
  }
  const auto kind = training_adaptation_from_string(value.value("adaptation", std::string {}));
  const auto has_lora = value.contains("lora") && !value.at("lora").is_null();
  const auto has_quantization =
    value.contains("quantization") && !value.at("quantization").is_null();
  if (kind == training_adaptation::full) {
    if (has_lora || has_quantization)
      throw std::invalid_argument("full training must not include LoRA or quantization fields");
    return full_training_config {};
  }
  if (!has_lora)
    throw std::invalid_argument("LoRA and QLoRA configurations require LoRA fields");
  const auto lora = lora_configuration_from_json(value.at("lora"));
  if (kind == training_adaptation::lora) {
    if (has_quantization)
      throw std::invalid_argument("LoRA configuration must not include quantization fields");
    return lora_training_config { .lora = lora };
  }
  if (!has_quantization)
    throw std::invalid_argument("QLoRA configuration requires quantization");
  return qlora_training_config {
    .lora = lora,
    .quantization = quantization_configuration_from_json(value.at("quantization")),
  };
}

inline training_hyperparameters training_hyperparameters_from_json(const nlohmann::json& value) {
  if (!value.is_object())
    throw std::invalid_argument("training hyperparameters must be a JSON object");
  constexpr const char* required_fields[] {
    "epochs",
    "maximum_steps",
    "batch_size",
    "gradient_accumulation_steps",
    "maximum_sequence_length",
    "learning_rate",
    "warmup_ratio",
    "seed",
    "numeric",
    "options",
  };
  for (const auto* field : required_fields) {
    if (!value.contains(field))
      throw std::invalid_argument(std::string("training hyperparameters require ") + field);
  }
  return {
    .epochs = value.value("epochs", std::size_t { 1 }),
    .maximum_steps = value.value("maximum_steps", std::size_t {}),
    .batch_size = value.value("batch_size", std::size_t { 1 }),
    .gradient_accumulation_steps = value.value("gradient_accumulation_steps", std::size_t { 1 }),
    .maximum_sequence_length = value.value("maximum_sequence_length", std::size_t { 2048 }),
    .learning_rate = value.value("learning_rate", 2e-4),
    .warmup_ratio = value.value("warmup_ratio", 0.0),
    .seed = value.value("seed", std::uint64_t { 42 }),
    .numeric = value.value("numeric", std::map<std::string, double> {}),
    .options = value.value("options", std::map<std::string, std::string> {}),
  };
}

inline model_artifact_manifest model_artifact_manifest_from_json(const nlohmann::json& value) {
  if (!value.is_object())
    throw std::invalid_argument("model artifact must be a JSON object");
  if (value.value("schema", std::string {}) != "wuwe.model-artifact.v1")
    throw std::invalid_argument("unsupported model artifact schema");
  require_json_integer_field(value, "created_at_unix_millis", "model artifact");
  require_json_field(value, "hyperparameters", nlohmann::json::value_t::object, "model artifact");
  model_artifact_manifest output {
    .id = value.value("id", std::string {}),
    .version = value.value("version", std::string {}),
    .objective = training_objective_from_string(value.value("objective", std::string {})),
    .adaptation = adaptation_configuration_from_json(value),
    .uri = value.value("uri", std::string {}),
    .digest = value.value("digest", std::string {}),
    .format = value.value("format", std::string {}),
    .base_model = model_reference_from_json(value.value("base_model", nlohmann::json::object())),
    .tokenizer = value.value("tokenizer", std::string {}),
    .chat_template = value.value("chat_template", std::string {}),
    .hyperparameters =
      training_hyperparameters_from_json(value.value("hyperparameters", nlohmann::json::object())),
    .provider = value.value("provider", std::string {}),
    .provider_job_id = value.value("provider_job_id", std::string {}),
    .metadata = value.value("metadata", std::map<std::string, std::string> {}),
  };
  const auto dataset = value.value("dataset", nlohmann::json::object());
  if (!dataset.is_object())
    throw std::invalid_argument("model artifact dataset lineage must be a JSON object");
  output.dataset_id = dataset.value("id", std::string {});
  output.dataset_version = dataset.value("version", std::string {});
  output.dataset_digest = dataset.value("digest", std::string {});
  output.created_at = training_time_from_json(value, "created_at_unix_millis");
  validate_model_artifact(output);
  return output;
}

inline training_checkpoint training_checkpoint_from_json(const nlohmann::json& value) {
  if (!value.is_object())
    throw std::invalid_argument("training checkpoint must be a JSON object");
  require_json_field(value, "id", nlohmann::json::value_t::string, "training checkpoint");
  require_json_field(value, "uri", nlohmann::json::value_t::string, "training checkpoint");
  require_json_field(value, "digest", nlohmann::json::value_t::string, "training checkpoint");
  require_json_field(
    value, "step", nlohmann::json::value_t::number_unsigned, "training checkpoint");
  require_json_integer_field(value, "created_at_unix_millis", "training checkpoint");
  training_checkpoint output {
    .id = value.value("id", std::string {}),
    .uri = value.value("uri", std::string {}),
    .digest = value.value("digest", std::string {}),
    .step = value.value("step", std::size_t {}),
    .metadata = value.value("metadata", std::map<std::string, std::string> {}),
  };
  output.created_at = training_time_from_json(value, "created_at_unix_millis");
  validate_training_checkpoint(output);
  return output;
}

inline training_dataset_manifest training_dataset_manifest_from_json(const nlohmann::json& value) {
  if (!value.is_object())
    throw std::invalid_argument("training dataset manifest must be a JSON object");
  require_json_field(value, "format", nlohmann::json::value_t::string, "training dataset manifest");
  require_json_integer_field(value, "created_at_unix_millis", "training dataset manifest");
  training_dataset_manifest output {
    .id = value.value("id", std::string {}),
    .version = value.value("version", std::string {}),
    .format = value.value("format", std::string {}),
    .uri = value.value("uri", std::string {}),
    .digest = value.value("digest", std::string {}),
    .example_count = value.value("example_count", std::size_t {}),
    .message_count = value.value("message_count", std::size_t {}),
    .tokenizer = value.value("tokenizer", std::string {}),
    .chat_template = value.value("chat_template", std::string {}),
    .metadata = value.value("metadata", std::map<std::string, std::string> {}),
  };
  output.created_at = training_time_from_json(value, "created_at_unix_millis");
  return output;
}

inline training_request training_request_from_json(const nlohmann::json& value) {
  if (!value.is_object())
    throw std::invalid_argument("training request must be a JSON object");
  if (value.value("schema", std::string {}) != "wuwe.training-request.v1")
    throw std::invalid_argument("unsupported training request schema");
  require_json_field(value, "hyperparameters", nlohmann::json::value_t::object, "training request");
  require_json_field(value, "resources", nlohmann::json::value_t::object, "training request");
  training_request output {
    .id = value.value("id", std::string {}),
    .idempotency_key = value.value("idempotency_key", std::string {}),
    .target = value.value("target", std::string {}),
    .objective = training_objective_from_string(value.value("objective", std::string {})),
    .adaptation = adaptation_configuration_from_json(value),
    .base_model = model_reference_from_json(value.value("base_model", nlohmann::json::object())),
    .dataset =
      training_dataset_manifest_from_json(value.value("dataset", nlohmann::json::object())),
    .hyperparameters =
      training_hyperparameters_from_json(value.value("hyperparameters", nlohmann::json::object())),
    .output_uri = value.value("output_uri", std::string {}),
    .metadata = value.value("metadata", std::map<std::string, std::string> {}),
  };
  const auto resources = value.value("resources", nlohmann::json::object());
  constexpr const char* required_resource_fields[] {
    "accelerators",
    "accelerator_type",
    "memory_gib",
    "maximum_runtime_seconds",
    "queue",
    "metadata",
  };
  for (const auto* field : required_resource_fields) {
    if (!resources.contains(field))
      throw std::invalid_argument(std::string("training resources require ") + field);
  }
  output.resources = {
    .accelerators = resources.value("accelerators", std::size_t { 1 }),
    .accelerator_type = resources.value("accelerator_type", std::string {}),
    .memory_gib = resources.value("memory_gib", std::size_t {}),
    .maximum_runtime =
      std::chrono::seconds(resources.value("maximum_runtime_seconds", std::int64_t {})),
    .queue = resources.value("queue", std::string {}),
    .metadata = resources.value("metadata", std::map<std::string, std::string> {}),
  };
  validate_training_request(output);
  return output;
}

inline training_provider_snapshot training_provider_snapshot_from_json(
  const nlohmann::json& value) {
  if (!value.is_object())
    throw std::invalid_argument("training provider response must be a JSON object");
  require_json_field(
    value, "provider_job_id", nlohmann::json::value_t::string, "training provider response");
  require_json_field(
    value, "status", nlohmann::json::value_t::string, "training provider response");
  require_json_number_field(value, "progress", "training provider response");
  require_json_field(
    value, "current_step", nlohmann::json::value_t::number_unsigned, "training provider response");
  training_provider_snapshot output {
    .provider_job_id = value.value("provider_job_id", std::string {}),
    .status = training_job_status_from_string(value.value("status", std::string {})),
    .progress = value.value("progress", 0.0),
    .current_step = value.value("current_step", std::size_t {}),
    .status_message = value.value("status_message", std::string {}),
    .error_code = value.value("error_code", std::string {}),
    .error = value.value("error", std::string {}),
    .metadata = value.value("metadata", std::map<std::string, std::string> {}),
  };
  if (value.contains("total_steps") && !value.at("total_steps").is_null())
    output.total_steps = value.at("total_steps").get<std::size_t>();
  if (value.contains("metrics")) {
    if (!value.at("metrics").is_array())
      throw std::invalid_argument("training metrics must be a JSON array");
    for (const auto& item : value.at("metrics")) {
      if (!item.is_object())
        throw std::invalid_argument("training metric must be a JSON object");
      require_json_field(item, "step", nlohmann::json::value_t::number_unsigned, "training metric");
      require_json_field(item, "name", nlohmann::json::value_t::string, "training metric");
      require_json_number_field(item, "value", "training metric");
      require_json_integer_field(item, "observed_at_unix_millis", "training metric");
      output.metrics.push_back({
        .step = item.value("step", std::size_t {}),
        .name = item.value("name", std::string {}),
        .value = item.value("value", 0.0),
        .observed_at = training_time_from_json(item, "observed_at_unix_millis"),
        .metadata = item.value("metadata", std::map<std::string, std::string> {}),
      });
    }
  }
  if (value.contains("latest_checkpoint") && !value.at("latest_checkpoint").is_null())
    output.latest_checkpoint = training_checkpoint_from_json(value.at("latest_checkpoint"));
  if (value.contains("artifact") && !value.at("artifact").is_null())
    output.artifact = model_artifact_manifest_from_json(value.at("artifact"));
  validate_provider_snapshot(output);
  return output;
}

inline training_submission_record training_submission_record_from_json(
  const nlohmann::json& value) {
  if (!value.is_object())
    throw std::invalid_argument("training submission record must be a JSON object");
  require_json_field(
    value, "idempotency_key", nlohmann::json::value_t::string, "training submission record");
  require_json_field(
    value, "request_id", nlohmann::json::value_t::string, "training submission record");
  require_json_field(
    value, "request_digest", nlohmann::json::value_t::string, "training submission record");
  require_json_field(
    value, "snapshot", nlohmann::json::value_t::object, "training submission record");
  training_submission_record output {
    .idempotency_key = value.at("idempotency_key").get<std::string>(),
    .request_id = value.at("request_id").get<std::string>(),
    .request_digest = value.at("request_digest").get<std::string>(),
    .snapshot = training_provider_snapshot_from_json(value.at("snapshot")),
  };
  validate_training_submission_record(output);
  return output;
}

inline nlohmann::json training_checkpoint_to_json(const training_checkpoint& value) {
  return {
    { "id", value.id },
    { "uri", value.uri },
    { "digest", value.digest },
    { "step", value.step },
    { "created_at_unix_millis",
      std::chrono::duration_cast<std::chrono::milliseconds>(value.created_at.time_since_epoch())
        .count() },
    { "metadata", value.metadata },
  };
}

inline nlohmann::json training_provider_snapshot_to_json(const training_provider_snapshot& value) {
  auto metrics = nlohmann::json::array();
  for (const auto& metric : value.metrics) {
    metrics.push_back({
      { "step", metric.step },
      { "name", metric.name },
      { "value", metric.value },
      { "observed_at_unix_millis",
        std::chrono::duration_cast<std::chrono::milliseconds>(metric.observed_at.time_since_epoch())
          .count() },
      { "metadata", metric.metadata },
    });
  }
  nlohmann::json checkpoint;
  if (value.latest_checkpoint)
    checkpoint = training_checkpoint_to_json(*value.latest_checkpoint);
  nlohmann::json artifact;
  if (value.artifact)
    artifact = model_artifact_manifest_to_json(*value.artifact);
  return {
    { "provider_job_id", value.provider_job_id },
    { "status", to_string(value.status) },
    { "progress", value.progress },
    { "current_step", value.current_step },
    { "total_steps", value.total_steps },
    { "status_message", value.status_message },
    { "error_code", value.error_code },
    { "error", value.error },
    { "metrics", std::move(metrics) },
    { "latest_checkpoint", std::move(checkpoint) },
    { "artifact", std::move(artifact) },
    { "metadata", value.metadata },
  };
}

inline nlohmann::json training_submission_record_to_json(const training_submission_record& value) {
  return {
    { "idempotency_key", value.idempotency_key },
    { "request_id", value.request_id },
    { "request_digest", value.request_digest },
    { "snapshot", training_provider_snapshot_to_json(value.snapshot) },
  };
}

inline nlohmann::json training_job_record_to_json(const training_job& value) {
  return {
    { "schema", "wuwe.training-job.v1" },
    { "id", value.id },
    { "provider", value.provider },
    { "tenant_id", value.tenant_id },
    { "workspace_id", value.workspace_id },
    { "revision", value.revision },
    { "request", training_request_to_json(value.request) },
    { "provider_state", training_provider_snapshot_to_json(value.provider_state) },
    { "created_at_unix_millis",
      std::chrono::duration_cast<std::chrono::milliseconds>(value.created_at.time_since_epoch())
        .count() },
    { "updated_at_unix_millis",
      std::chrono::duration_cast<std::chrono::milliseconds>(value.updated_at.time_since_epoch())
        .count() },
    { "metadata", value.metadata },
  };
}

inline training_job training_job_record_from_json(const nlohmann::json& value) {
  if (!value.is_object() || value.value("schema", std::string {}) != "wuwe.training-job.v1")
    throw std::invalid_argument("unsupported training job record schema");
  require_json_integer_field(value, "created_at_unix_millis", "training job record");
  require_json_integer_field(value, "updated_at_unix_millis", "training job record");
  training_job output {
    .id = value.value("id", std::string {}),
    .provider = value.value("provider", std::string {}),
    .tenant_id = value.value("tenant_id", std::string {}),
    .workspace_id = value.value("workspace_id", std::string {}),
    .revision = value.value("revision", std::uint64_t {}),
    .request = training_request_from_json(value.value("request", nlohmann::json::object())),
    .provider_state =
      training_provider_snapshot_from_json(value.value("provider_state", nlohmann::json::object())),
    .metadata = value.value("metadata", std::map<std::string, std::string> {}),
  };
  output.created_at = std::chrono::system_clock::time_point(
    std::chrono::milliseconds(value.value("created_at_unix_millis", std::int64_t {})));
  output.updated_at = std::chrono::system_clock::time_point(
    std::chrono::milliseconds(value.value("updated_at_unix_millis", std::int64_t {})));
  if (output.id.empty() || output.provider.empty() || output.revision == 0)
    throw std::invalid_argument("training job record is incomplete");
  return output;
}

} // namespace wuwe::agent::training

#endif // WUWE_AGENT_TRAINING_TRAINING_CODEC_HPP
