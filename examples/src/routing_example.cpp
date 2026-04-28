#include <windows.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include <wuwe/agent/core/message.hpp>
#include <wuwe/agent/orchestration/flow_primitives.hpp>
#include <wuwe/wuwe.h>

struct routing_result {
  std::string lane;
  std::string action;
};

// clang-format off
auto classify(const std::string& request) {
  using wuwe::says;
  return wuwe::make_message()
    << ("system" <says> "You are an incident triage router.\n"
                          "Classify each request into exactly one label:\n"
                          "- security: account takeover, suspicious login, data leak, phishing\n"
                          "- infra: outage, timeout, high latency, service unavailable\n"
                          "- data: wrong report, missing records, dashboard mismatch\n"
                          "- unclear: none of the above or not enough context\n\n"
                          "Return ONLY one word: security, infra, data, or unclear.")
    << ("user" <says> request);
}

void log_route_label(const wuwe::llm_response& response) {
  wuwe::println("router label: {}", response.content);
}

routing_result security_lane(const wuwe::llm_response&) {
  return {
    .lane = "Security Team",
    .action = "Open security incident and isolate affected account.",
  };
}

routing_result infra_lane(const wuwe::llm_response&) {
  return {
    .lane = "Infrastructure Team",
    .action = "Create infra ticket and collect service metrics.",
  };
}

routing_result data_lane(const wuwe::llm_response&) {
  return {
    .lane = "Data Team",
    .action = "Create data-quality task and validate upstream pipeline.",
  };
}

routing_result unclear_lane(const wuwe::llm_response& response) {
  return { .lane = "Manual Triage",
    .action = "Need more details. Model label: '" + response.content + "'." };
}

int main() {
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);

  wuwe::llm_config config {
    .base_url = "https://openrouter.ai/api",
    .model = "inclusionai/ling-2.6-1t:free",
    .timeout = 30000,
  };

  wuwe::llm_client_factory factory;
  auto client = factory.create_shared("OpenRouter", config);

  auto flow = client
            | classify
            | wuwe::tee(log_route_label)
            | wuwe::route(
                wuwe::when(
                  [](const wuwe::llm_response& response) { return response.content == "security"; },
                  security_lane),
                wuwe::when(
                  [](const wuwe::llm_response& response) { return response.content == "infra"; },
                  infra_lane),
                wuwe::when(
                  [](const wuwe::llm_response& response) { return response.content == "data"; },
                  data_lane),
                wuwe::otherwise(unclear_lane)
              );
  // clang-format on

  const std::vector<std::string> requests {
    "A user reports suspicious sign-ins from two unknown countries.",
    "Checkout API returns 503 in our production region.",
    "Sales dashboard shows zero orders for yesterday, but orders exist.",
    "Can you summarize this week's product roadmap?"
  };

  for (const auto& request : requests) {
    wuwe::println("Request: {}", request);
    const auto result = flow.invoke(request);
    wuwe::println("Lane: {} ", result.lane);
    wuwe::println("Action: {}", result.action);
    wuwe::println("================================");
  }
}
