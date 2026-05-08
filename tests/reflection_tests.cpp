#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <wuwe/agent/reflection/reflection.hpp>

namespace {

using namespace wuwe;
using namespace wuwe::agent::reflection;

void require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void require_throws(auto&& callback, const std::string& message) {
  try {
    callback();
  }
  catch (const std::exception& ex) {
    require(std::string(ex.what()).find(message) != std::string::npos,
      std::string("exception message contains ") + message);
    return;
  }
  throw std::runtime_error("expected exception containing: " + message);
}

class fake_llm_client final : public llm_client {
public:
  explicit fake_llm_client(std::string content) : content_(std::move(content)) {
  }

  llm_response complete(const llm_request& request) override {
    last_request = request;
    return { .content = content_ };
  }

  std::string content_;
  llm_request last_request;
};

void rule_reflector_passes_valid_output() {
  rule_reflector reflector({
    .reject_empty_output = true,
    .min_output_chars = 4,
    .required_substrings = { "answer" },
  });

  const auto result = reflector.reflect({
    .task = "Check answer",
    .candidate_output = "final answer",
  });

  require(result.passed, "rule reflector passes valid output");
  require(result.recommended_action == reflection_action::pass, "valid output recommends pass");
}

void rule_reflector_flags_quality_issues() {
  rule_reflector reflector({
    .require_json_object = true,
    .forbidden_substrings = { "secret" },
  });

  const auto result = reflector.reflect({
    .task = "Check JSON",
    .candidate_output = "not json with secret",
  });

  require(!result.passed, "rule reflector fails invalid output");
  require(result.recommended_action == reflection_action::escalate,
    "critical forbidden text escalates");
  require(result.issues.size() == 2, "rule reflector reports both issues");
}

void llm_reflector_parses_structured_json() {
  fake_llm_client client(
    "```json\n"
    "{\"passed\":false,\"score\":0.62,\"recommended_action\":\"revise\","
    "\"issues\":[{\"severity\":\"warning\",\"code\":\"thin\",\"message\":\"Needs detail\","
    "\"evidence\":\"short\",\"suggestion\":\"add detail\",\"metadata\":{}}],"
    "\"revised_output\":\"better answer\",\"metadata\":{\"source\":\"llm\"}}\n"
    "```");

  llm_reflector reflector(client);
  const auto result = reflector.reflect({
    .task = "Review answer",
    .candidate_output = "answer",
    .rubric = {
      .criteria = { { .name = "helpfulness", .description = "Useful answer" } },
    },
  });

  require(!result.passed, "llm reflector preserves failed result");
  require(result.score == 0.62, "llm reflector parses score");
  require(result.recommended_action == reflection_action::revise, "llm reflector parses action");
  require(result.revised_output == "better answer", "llm reflector parses revised output");
  require(client.last_request.messages.back().content.find("Rubric") != std::string::npos,
    "llm reflector prompt includes rubric");
}

void composite_reflector_merges_results() {
  auto first = std::make_shared<rule_reflector>(rule_reflector_options {
    .required_substrings = { "required" },
  });
  auto second = std::make_shared<rule_reflector>(rule_reflector_options {
    .forbidden_substrings = { "forbidden" },
  });

  composite_reflector reflector;
  reflector.add(first).add(second);

  const auto result = reflector.reflect({
    .task = "Composite",
    .candidate_output = "forbidden",
  });

  require(!result.passed, "composite reflector fails when children fail");
  require(result.recommended_action == reflection_action::escalate,
    "composite reflector keeps strongest action");
  require(result.issues.size() == 2, "composite reflector merges issues");
}

void policy_maps_result_to_action() {
  reflection_policy policy {
    .pass_threshold = 0.8,
    .revise_threshold = 0.5,
    .retry_threshold = 0.2,
  };

  reflection_result result {
    .passed = false,
    .score = 0.6,
    .recommended_action = reflection_action::retry,
    .revised_output = "revision",
  };

  require(policy.action_for(result) == reflection_action::revise,
    "policy maps medium score with revision to revise");
  require(action_for(policy, result) == reflection_action::revise,
    "free policy helper delegates to policy engine");

  result.score = 0.1;
  result.revised_output.clear();
  require(policy.action_for(result) == reflection_action::block,
    "policy maps low score to block");

  auto applied = reflection_policy_engine(policy).apply(result);
  require(applied.recommended_action == reflection_action::block, "policy engine applies action");
  require(!applied.passed, "policy engine updates pass status");

  result.recommended_action = reflection_action::replan;
  require(policy.action_for(result) == reflection_action::replan,
    "policy preserves explicit replanning action");
}

void result_normalizer_and_merger_are_reusable() {
  reflection_result dirty {
    .passed = true,
    .score = 2.5,
    .recommended_action = reflection_action::revise,
    .issues = { { .severity = reflection_severity::warning, .code = "warn" } },
    .revised_output = "revision",
  };

  const auto normalized = reflection_result_normalizer({
    .allow_revision = false,
  }).normalize(dirty);
  require(!normalized.passed, "normalizer fails result with issues");
  require(normalized.score == 1.0, "normalizer clamps score");
  require(normalized.recommended_action == reflection_action::retry,
    "normalizer respects rubric revision policy");
  require(normalized.revised_output.empty(), "normalizer clears disallowed revision");

  const auto merged = reflection_result_merger()
                        .add(reflection_result::pass())
                        .add(reflection_result::fail(reflection_action::retry,
                          { .severity = reflection_severity::error, .code = "bad" },
                          0.4))
                        .finish();
  require(!merged.passed, "merger fails when any result fails");
  require(merged.recommended_action == reflection_action::retry, "merger keeps strongest action");
  require(merged.score == 0.4, "merger keeps lowest score");
}

void reflection_store_round_trips_records() {
  reflection_record record {
    .id = "r1",
    .request = { .task = "Store", .candidate_output = "ok" },
    .result = reflection_result::pass(),
  };

  in_memory_reflection_store memory_store;
  memory_store.save(record);
  require(memory_store.load("r1").has_value(), "in-memory reflection store loads record");
  require(memory_store.list().size() == 1, "in-memory reflection store lists record");
  require(memory_store.erase("r1"), "in-memory reflection store erases record");

  const auto path = std::filesystem::temp_directory_path() / "wuwe-reflection-tests-store.json";
  std::filesystem::remove(path);
  file_reflection_store file_store(path);
  file_store.save(record);
  require(file_store.load("r1").has_value(), "file reflection store loads record");
  require(file_store.erase("r1"), "file reflection store erases record");
  std::filesystem::remove(path);
}

void reflection_runner_applies_policy_and_records() {
  auto reflector = std::make_shared<rule_reflector>(rule_reflector_options {
    .required_substrings = { "required" },
  });
  in_memory_reflection_store store;
  std::vector<reflection_event_type> events;

  reflection_runner runner({
    .reflector = reflector,
    .policy = { .pass_threshold = 0.8 },
    .store = &store,
    .observer = [&](const reflection_event& event) {
      events.push_back(event.type);
    },
  });

  const auto run = runner.run({
    .task = "Runner",
    .candidate_output = "missing",
  });

  require(!run.result.passed, "runner preserves failed result");
  require(run.result.recommended_action == reflection_action::retry ||
            run.result.recommended_action == reflection_action::revise,
    "runner applies policy-compatible action");
  require(store.load(run.record.id).has_value(), "runner stores reflection record");
  require(events.size() == 2, "runner emits started and completed events");
}

void codec_round_trips_result() {
  reflection_result result {
    .passed = false,
    .score = 0.25,
    .recommended_action = reflection_action::block,
    .issues = { { .severity = reflection_severity::error, .code = "bad", .message = "Bad" } },
  };

  const auto json = reflection_result_to_json(result);
  const auto restored = reflection_codec::result_from_json(json);
  require(!restored.passed, "codec preserves pass status");
  require(restored.recommended_action == reflection_action::block, "codec preserves action");
  require(restored.issues.front().code == "bad", "codec preserves issue");
}

} // namespace

int main() {
  rule_reflector_passes_valid_output();
  rule_reflector_flags_quality_issues();
  llm_reflector_parses_structured_json();
  composite_reflector_merges_results();
  policy_maps_result_to_action();
  result_normalizer_and_merger_are_reusable();
  reflection_store_round_trips_records();
  reflection_runner_applies_policy_and_records();
  codec_round_trips_result();
}
