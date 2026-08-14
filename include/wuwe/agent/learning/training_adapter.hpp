#ifndef WUWE_AGENT_LEARNING_TRAINING_ADAPTER_HPP
#define WUWE_AGENT_LEARNING_TRAINING_ADAPTER_HPP

#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <wuwe/agent/learning/adaptation_core.hpp>
#include <wuwe/agent/learning/learning_core.hpp>
#include <wuwe/agent/training/training_json.hpp>
#include <wuwe/common/sha256.hpp>

namespace wuwe::agent::learning {

struct sft_dataset_builder_options {
  std::string dataset_id;
  std::string version;
  std::string uri;
  std::string digest;
  std::string system_prompt;
  std::string tokenizer;
  std::string chat_template;
  bool prefer_expected_output { true };
  bool include_without_expected_output { false };
  std::map<std::string, std::string> metadata;
};

using experience_training_mapper =
  std::function<std::optional<training::training_example>(const experience_record&)>;

inline training::training_dataset build_sft_dataset(
  const std::vector<experience_record>& experiences, sft_dataset_builder_options options,
  experience_training_mapper mapper = {}) {
  if (options.dataset_id.empty() || options.version.empty())
    throw std::invalid_argument("SFT dataset builder requires dataset id and version");
  training::training_dataset output;
  output.manifest = { .id = std::move(options.dataset_id),
    .version = std::move(options.version),
    .uri = std::move(options.uri),
    .digest = std::move(options.digest),
    .tokenizer = std::move(options.tokenizer),
    .chat_template = std::move(options.chat_template),
    .metadata = std::move(options.metadata) };
  for (const auto& experience : experiences) {
    std::optional<training::training_example> example;
    if (mapper)
      example = mapper(experience);
    else {
      const auto response = options.prefer_expected_output && !experience.expected_output.empty()
                              ? experience.expected_output
                              : experience.output;
      if (experience.input.empty() || response.empty() ||
          (experience.expected_output.empty() && !options.include_without_expected_output))
        continue;
      training::training_example generated { .id = experience.id,
        .source = experience.source,
        .source_run_id = experience.source_run_id,
        .metadata = experience.metadata };
      if (!options.system_prompt.empty())
        generated.messages.push_back(
          { .role = training::training_message_role::system, .content = options.system_prompt });
      generated.messages.push_back(
        { .role = training::training_message_role::user, .content = experience.input });
      generated.messages.push_back({ .role = training::training_message_role::assistant,
        .content = response,
        .trainable = true });
      generated.metadata.try_emplace("experience_id", experience.id);
      generated.metadata.try_emplace("feedback_kind", to_string(experience.feedback_type));
      example = std::move(generated);
    }
    if (!example)
      continue;
    if (example->id.empty()) {
      auto semantic = training::training_example_to_json(*example);
      semantic["id"] = "";
      example->id = "training-example-" + common::sha256_hex(semantic.dump());
    }
    training::validate_training_example(*example);
    output.manifest.message_count += example->messages.size();
    output.examples.push_back(std::move(*example));
  }
  output.manifest.example_count = output.examples.size();
  const auto computed_digest = training::training_dataset_digest(output);
  if (!output.manifest.digest.empty() && output.manifest.digest != computed_digest)
    throw std::invalid_argument("declared training dataset digest does not match its examples");
  output.manifest.digest = computed_digest;
  training::validate_training_dataset(output);
  return output;
}

inline learning_candidate learning_candidate_from_training_job(
  const training::training_job& job, std::string parent_version = {}) {
  if (job.provider_state.status != training::training_job_status::succeeded ||
      !job.provider_state.artifact) {
    throw std::invalid_argument("only successful training jobs can become learning candidates");
  }
  training::validate_training_job(job);
  const auto& artifact = *job.provider_state.artifact;
  return {
    .id = make_learning_id("model-candidate"),
    .kind = learning_change_kind::model,
    .target = job.request.target,
    .parent_version = std::move(parent_version),
    .proposed_version = artifact.version,
    .artifact = training::model_artifact_manifest_to_json(artifact),
    .rationale = "promote validated model artifact from training job " + job.id,
    .metadata = {
      { "artifact_kind",
        training::adaptation_kind(job.request.adaptation) == training::training_adaptation::full
          ? "model" : "model_adapter" },
      { "training_job_id", job.id },
      { "training_provider", job.provider },
      { "provider_job_id", job.provider_state.provider_job_id },
      { "dataset_digest", artifact.dataset_digest },
      { "artifact_digest", artifact.digest },
    },
  };
}

} // namespace wuwe::agent::learning

#endif // WUWE_AGENT_LEARNING_TRAINING_ADAPTER_HPP
