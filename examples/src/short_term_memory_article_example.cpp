#include <wuwe/agent/memory/memory_context.hpp>
#include <wuwe/wuwe.h>

std::string summarize_article_setup(
  const std::vector<wuwe::agent::memory::memory_record>& records) {
  std::string summary = "Article setup: ";
  for (std::size_t index = 0; index < records.size(); ++index) {
    if (index != 0) {
      summary += " ";
    }
    summary += records[index].content;
  }
  return summary;
}

int main() {
  namespace memory = wuwe::agent::memory;

  memory::memory_policy policy;
  policy.max_recent_messages = 6;
  policy.max_working_records = 4;
  policy.max_summary_records = 2;
  policy.max_long_term_records = 0;
  policy.max_memory_chars = 2200;

  memory::memory_context context(policy);
  context.set_scope({
    .user_id = "article-reader",
    .application_id = "memory-article",
    .conversation_id = "session-short-term",
    .agent_id = "assistant",
  });

  context.observe({
    .role = "user",
    .content = "We are writing an article about memory management in Wuwe.",
  });
  context.observe({
    .role = "assistant",
    .content = "The article should first explain why short-term and long-term memory differ.",
  });
  context.observe({
    .role = "user",
    .content = "Use a concrete debugging session as the short-term memory example.",
  });

  context.remember_working("Current task: prepare the next paragraph about short-term memory.",
    { { "topic", "article-draft" } });
  context.remember_working(
    "The example should show temporary conversation state, not persistent user facts.",
    { { "topic", "article-draft" } });

  context.summarize_conversation(
    {
      .scope = context.scope(),
      .max_source_records = 3,
      .erase_source_records = false,
      .metadata = { { "topic", "article-summary" } },
    },
    summarize_article_setup);

  auto request = wuwe::make_message()
                 << ("system" < wuwe::says > "You are helping draft a technical article.")
                 << ("user" < wuwe::says > "Write the next paragraph about short-term memory.");

  const auto augmented =
    context.augment(std::move(request), "next paragraph about short-term memory");

  wuwe::println("Short-term memory example");
  wuwe::println(
    "This memory exists inside the current session and is assembled before the LLM call.\n");
  wuwe::println("Augmented request:");
  for (const auto& message : augmented.messages) {
    wuwe::println("[{}] {}\n", message.role, message.content);
  }
}
