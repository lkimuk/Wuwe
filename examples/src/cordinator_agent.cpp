#include <windows.h>

#include <algorithm>
#include <cctype>
#include <format>
#include <string>
#include <string_view>

#include <wuwe/agent/core/message.hpp>
#include <wuwe/agent/orchestration/flow_primitives.hpp>
#include <wuwe/wuwe.h>

namespace {

struct coordinator_context {
  std::string request;
  std::string decision;
  std::string output;
};

std::string trim_lower(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return {};
  }

  const auto last = value.find_last_not_of(" \t\r\n");
  value = value.substr(first, last - first + 1);
  std::ranges::transform(
    value, value.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return value;
}

std::string booking_handler(std::string_view request) {
  wuwe::println("\n--- DELEGATING TO BOOKING HANDLER ---");
  return std::format(
    "Booking Handler processed request: '{}'. Result: Simulated booking action.", request);
}

std::string info_handler(std::string_view request) {
  wuwe::println("\n--- DELEGATING TO INFO HANDLER ---");
  return std::format(
    "Info Handler processed request: '{}'. Result: Simulated information retrieval.", request);
}

std::string unclear_handler(std::string_view request) {
  wuwe::println("\n--- HANDLING UNCLEAR REQUEST ---");
  return std::format("Coordinator could not delegate request: '{}'. Please clarify.", request);
}

std::string router_prompt(std::string_view request) {
  return std::format("Analyze the user's request and choose exactly one specialist handler.\n"
                     "- Booking flights or hotels: booker\n"
                     "- General information questions: info\n"
                     "- Unclear or unrelated requests: unclear\n\n"
                     "Only output one word: booker, info, or unclear.\n\n"
                     "Request: {}",
    request);
}

auto route_to_handler() {
  return wuwe::if_else(
    [](const coordinator_context& context) { return context.decision == "booker"; },
    [](coordinator_context context) {
      context.output = booking_handler(context.request);
      return context;
    },
    wuwe::if_else([](const coordinator_context& context) { return context.decision == "info"; },
      [](coordinator_context context) {
        context.output = info_handler(context.request);
        return context;
      },
      [](coordinator_context context) {
        context.output = unclear_handler(context.request);
        return context;
      }));
}

} // namespace

int main() {
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);

  wuwe::llm_config config {
    .base_url = "https://openrouter.ai/api",
    .model = "openai/gpt-oss-120b:free",
    .timeout = 30000,
  };

  wuwe::llm_client_factory factory;
  auto client = factory.create_shared("OpenRouter", config);

  using namespace wuwe::literals::message_literals;
  auto decision = [](const std::string& input) {
    // return wuwe::make_chat_message()
    //        << ("system" <says>
    //             "Analyze the user's request and determine which specialist handler should "
    //             "process it."
    //             "- If the request is related to booking flights or hotels, output 'booker'."
    //             "- For all other general information questions, output 'info'."
    //             "- If the request is unclear or doesn't fit either category, output 'unclear'.\n"
    //             "ONLY output one word: 'booker', 'info', or 'unclear'.")
    //        << ("user" <says> input);
    return wuwe::make_message()
           << "system"_msg(
                "Analyze the user's request and determine which specialist handler should "
                "process it."
                "- If the request is related to booking flights or hotels, output 'booker'."
                "- For all other general information questions, output 'info'."
                "- If the request is unclear or doesn't fit either category, output 'unclear'.\n"
                "ONLY output one word: 'booker', 'info', or 'unclear'.")
           << "user"_msg(input);
  };

  // clang-format off
  auto flow = client
            | decision;

  // clang-format on
  flow.invoke("Book me a flight to London.");
  // auto invoke = [client](const std::string& request) {
  //   auto agent = client | [request](const std::string&) { return router_prompt(request); } |
  //                [request](const wuwe::llm_response& response) {
  //                  return coordinator_context {
  //                    .request = request,
  //                    .decision = trim_lower(response.content),
  //                  };
  //                } |
  //                wuwe::tee([](const coordinator_context& context) {
  //                  wuwe::println("coordinator decision: {}", context.decision);
  //                }) |
  //                route_to_handler() |
  //                wuwe::recover([](const std::exception& e) {
  //                  return coordinator_context {
  //                    .output = std::string("Coordinator failed: ") + e.what(),
  //                  };
  //                });

  //   return agent.invoke(request);
  // };

  // wuwe::println("--- Running with a booking request ---");
  // wuwe::println("Final Result A: {}", invoke("Book me a flight to London.").output);

  // wuwe::println("\n--- Running with an info request ---");
  // wuwe::println("Final Result B: {}", invoke("What is the capital of Italy?").output);

  // wuwe::println("\n--- Running with an unclear request ---");
  // wuwe::println("Final Result C: {}", invoke("Tell me about quantum physics.").output);
}
