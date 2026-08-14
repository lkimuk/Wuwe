#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <wuwe/agent/learning/learning.hpp>
#include <wuwe/agent/training/training.hpp>

int main() {
  namespace learning = wuwe::agent::learning;
  namespace training = wuwe::agent::training;

  learning::in_memory_experience_store experiences;
  experiences.add({
    .target = "support.model",
    .source = "reviewed-production-feedback",
    .source_run_id = "agent-run-42",
    .input = "How do I rotate an API token?",
    .output = "Create another token.",
    .expected_output =
      "Open Settings, revoke the old token, create a replacement, and update the secret store.",
    .feedback_type = learning::feedback_kind::correction,
  });

  const auto dataset = learning::build_sft_dataset(experiences.query({
                                                     .target = "support.model",
                                                   }),
    {
      .dataset_id = "support-sft",
      .version = "2026-08-13",
      .uri = "s3://datasets/support-sft/2026-08-13.jsonl",
      .system_prompt = "Answer with grounded support procedures.",
      .tokenizer = "Qwen/Qwen3-8B@2f41c0d",
      .chat_template = "qwen3",
    });

  training::training_request request {
    .id = "support-lora-v2",
    .idempotency_key = "support-lora-v2",
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
      .digest = std::string(64, 'a'),
      .tokenizer = "Qwen/Qwen3-8B@2f41c0d",
      .chat_template = "qwen3",
    },
    .dataset = dataset.manifest,
    .hyperparameters = {
      .epochs = 2,
      .batch_size = 2,
      .gradient_accumulation_steps = 8,
      .maximum_sequence_length = 4096,
      .learning_rate = 1e-4,
    },
    .resources = {
      .accelerators = 1,
      .accelerator_type = "cuda",
      .memory_gib = 24,
    },
    .output_uri = "s3://models/support/adapter-v2",
  };

  // This deterministic provider demonstrates the lifecycle contract only. A production
  // host normally uses http_training_provider backed by TRL, LLaMA-Factory, Axolotl,
  // Kubernetes, or a managed training service.
  std::size_t inspections = 0;
  auto provider = std::make_shared<training::function_training_provider>(
    "example-provider",
    [](const training::training_request&, const training::training_context&) {
      return training::training_provider_snapshot {
        .provider_job_id = "remote-job-42",
        .status = training::training_job_status::queued,
      };
    },
    [&](const std::string& provider_job_id, const training::training_context&) {
      if (++inspections == 1) {
        return training::training_provider_snapshot {
          .provider_job_id = provider_job_id,
          .status = training::training_job_status::running,
          .progress = 0.5,
          .current_step = 50,
          .total_steps = 100,
          .metrics = { { .step = 50, .name = "train.loss", .value = 0.42 } },
        };
      }
      return training::training_provider_snapshot {
        .provider_job_id = provider_job_id,
        .status = training::training_job_status::succeeded,
        .progress = 1.0,
        .current_step = 100,
        .total_steps = 100,
        .artifact =
          training::model_artifact_manifest {
            .id = "support-adapter",
            .version = "adapter-v2",
            .objective = request.objective,
            .adaptation = request.adaptation,
            .uri = request.output_uri,
            .digest = std::string(64, 'c'),
            .format = "peft.safetensors",
            .base_model = request.base_model,
            .dataset_id = request.dataset.id,
            .dataset_version = request.dataset.version,
            .dataset_digest = request.dataset.digest,
            .tokenizer = request.base_model.tokenizer,
            .chat_template = request.base_model.chat_template,
            .hyperparameters = request.hyperparameters,
            .provider = "example-provider",
            .provider_job_id = provider_job_id,
          },
      };
    },
    [](const std::string& provider_job_id, const training::training_context&) {
      return training::training_provider_snapshot {
        .provider_job_id = provider_job_id,
        .status = training::training_job_status::cancelled,
      };
    });

  training::in_memory_training_job_store jobs;
  training::training_coordinator coordinator(provider,
    jobs,
    {
      .policy = training::trusted_training_policy(),
    });
  auto submitted = coordinator.submit(request);
  if (!submitted) {
    std::cerr << submitted.error_if()->message << '\n';
    return 1;
  }
  auto job = *submitted.value_if();
  for (int i = 0; i < 2; ++i) {
    auto refreshed = coordinator.refresh(job.id);
    if (!refreshed) {
      std::cerr << refreshed.error_if()->message << '\n';
      return 1;
    }
    job = *refreshed.value_if();
  }

  const auto candidate = learning::learning_candidate_from_training_job(job, "adapter-v1");
  std::cout << "dataset_sha256=" << dataset.manifest.digest << '\n'
            << "training_status=" << training::to_string(job.provider_state.status) << '\n'
            << "candidate_version=" << candidate.proposed_version << '\n';
}
