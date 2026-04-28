#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <string>

#include <wuwe/agent/memory/memory_context.hpp>
#include <wuwe/wuwe.h>

int main() {
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);

  wuwe::agent::memory::memory_policy policy {
    .max_recent_messages = 4,
    .max_working_records = 4,
    .max_long_term_records = 4,
    .max_memory_chars = 2000,
    .injection_header = "Relevant memory:",
  };

  wuwe::agent::memory::memory_context memory(policy);
  memory.set_scope({
    .user_id = "local-user",
    .application_id = "wuwe-memory-example",
    .conversation_id = "demo-session",
    .agent_id = "architect",
  });

  memory.observe({ .role = "user",
    .content = "We are designing Memory Management for Wuwe Agent Framework." });
  memory.observe({ .role = "assistant",
    .content = "The design should include short-term and long-term memory." });

  memory.remember_working(
    "Current implementation target is request augmentation before llm_client::complete.",
    { { "topic", "implementation" } });

  memory.remember_summary(
    "The current design uses memory_context to coordinate stores, policies, and context assembly.",
    { { "topic", "design" } });

  memory.remember_long_term(
    "The project prefers explicit C++20 APIs and minimal hidden runtime behavior.",
    memory.scope(),
    { { "topic", "api-style" }, { "source", "user" } });

  auto request = wuwe::make_message()
    << ("system" <wuwe::says> "You are a senior C++ framework engineer.")
    << ("user" <wuwe::says> "Design the public API for memory stores.");

  const auto augmented = memory.augment(std::move(request), "memory store public API");

  wuwe::println("Augmented request messages:");
  for (const auto& message : augmented.messages) {
    wuwe::println("[{}] {}", message.role, message.content);
  }
}
