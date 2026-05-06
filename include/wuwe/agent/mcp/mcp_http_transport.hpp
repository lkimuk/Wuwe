#ifndef WUWE_AGENT_MCP_HTTP_TRANSPORT_HPP
#define WUWE_AGENT_MCP_HTTP_TRANSPORT_HPP

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <wuwe/agent/mcp/mcp_server.hpp>

namespace wuwe::agent::mcp {

struct mcp_http_request {
  std::string method { "POST" };
  std::string body;
  std::vector<std::pair<std::string, std::string>> headers;
};

struct mcp_http_response {
  int status_code { 200 };
  std::string content_type { "application/json" };
  std::string body;
  std::vector<std::pair<std::string, std::string>> headers;
  std::vector<std::string> client_requests;
  std::vector<std::string> sse_events;

  std::string sse_body() const {
    std::string output;
    for (const auto& event : sse_events) {
      output += event;
    }
    return output;
  }
};

inline std::string mcp_sse_event(std::string data, std::string event = "message") {
  return "event: " + std::move(event) + "\n" +
         "data: " + std::move(data) + "\n\n";
}

class mcp_http_transport {
public:
  mcp_http_response handle(mcp_server& server, const mcp_http_request& request) const {
    if (request.method != "POST") {
      auto response = json_response(405, R"({"error":"method not allowed"})");
      response.headers.push_back({ "Allow", "POST" });
      return response;
    }

    if (!request_content_type_is_json(request)) {
      return json_response(415, R"({"error":"content-type must be application/json"})");
    }

    if (request.body.empty()) {
      return json_response(400, R"({"error":"request body must not be empty"})");
    }

    auto exchange = server.handle_message_exchange(request.body);
    auto response = json_response(exchange.response ? 200 : 202, exchange.response.value_or(std::string {}));
    response.status_code = exchange.response ? 200 : 202;
    response.client_requests = std::move(exchange.requests);
    response.sse_events.reserve(exchange.notifications.size());
    for (auto& notification : exchange.notifications) {
      response.sse_events.push_back(mcp_sse_event(std::move(notification)));
    }
    if (!response.sse_events.empty()) {
      response.headers.push_back({ "Cache-Control", "no-cache" });
    }
    return response;
  }

private:
  static mcp_http_response json_response(int status_code, std::string body) {
    return {
      .status_code = status_code,
      .content_type = "application/json",
      .body = std::move(body),
      .headers = { { "Content-Type", "application/json" } },
    };
  }

  static bool request_content_type_is_json(const mcp_http_request& request) {
    const auto content_type = header_value(request, "Content-Type");
    if (content_type.empty()) {
      return true;
    }
    return contains_ascii_case(content_type, "application/json");
  }

  static std::string header_value(const mcp_http_request& request, std::string_view name) {
    for (const auto& header : request.headers) {
      if (equals_ascii_case(header.first, name)) {
        return header.second;
      }
    }
    return {};
  }

  static bool contains_ascii_case(std::string_view value, std::string_view needle) {
    if (needle.empty() || needle.size() > value.size()) {
      return false;
    }
    for (std::size_t i = 0; i + needle.size() <= value.size(); ++i) {
      if (equals_ascii_case(value.substr(i, needle.size()), needle)) {
        return true;
      }
    }
    return false;
  }

  static bool equals_ascii_case(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
      return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
      char a = left[i];
      char b = right[i];
      if (a >= 'A' && a <= 'Z') {
        a = static_cast<char>(a - 'A' + 'a');
      }
      if (b >= 'A' && b <= 'Z') {
        b = static_cast<char>(b - 'A' + 'a');
      }
      if (a != b) {
        return false;
      }
    }
    return true;
  }
};

} // namespace wuwe::agent::mcp

#endif // WUWE_AGENT_MCP_HTTP_TRANSPORT_HPP
