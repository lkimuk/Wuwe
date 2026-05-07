#ifndef WUWE_AGENT_MCP_STDIO_TRANSPORT_HPP
#define WUWE_AGENT_MCP_STDIO_TRANSPORT_HPP

#include <cstdio>
#include <iostream>
#include <istream>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include <wuwe/agent/mcp/mcp_server.hpp>

namespace wuwe::agent::mcp {

class mcp_stdio_transport {
public:
  enum class message_framing {
    content_length,
    json_lines,
  };

  struct stdio_message {
    std::string body;
    message_framing framing { message_framing::content_length };
  };

  int run_stdio(mcp_server& server) const {
    configure_stdio_binary();
    return run(server, std::cin, std::cout);
  }

  int run(mcp_server& server, std::istream& input, std::ostream& output) const {
    while (true) {
      auto message = read_message(input);
      if (!message) {
        break;
      }

      const auto framing = message->framing;
      const auto exchange = server.handle_message_exchange(message->body);
      write_exchange(exchange, [&output, framing](const std::string& message) {
        write_message(output, message, framing);
      });
    }
    return 0;
  }

  int run_lines(mcp_server& server, std::istream& input, std::ostream& output) const {
    std::string line;
    while (std::getline(input, line)) {
      if (line.empty()) {
        continue;
      }

      const auto exchange = server.handle_message_exchange(line);
      write_exchange(exchange, [&output](const std::string& message) {
        output << message << '\n';
      });
      output.flush();
    }
    return 0;
  }

  static void write_framed_message(std::ostream& output, std::string_view message) {
    output << "Content-Length: " << message.size() << "\r\n\r\n";
    output.write(message.data(), static_cast<std::streamsize>(message.size()));
    output.flush();
  }

  static void write_line_message(std::ostream& output, std::string_view message) {
    output.write(message.data(), static_cast<std::streamsize>(message.size()));
    output << '\n';
    output.flush();
  }

  static void write_message(
    std::ostream& output,
    std::string_view message,
    message_framing framing) {
    if (framing == message_framing::json_lines) {
      write_line_message(output, message);
      return;
    }
    write_framed_message(output, message);
  }

  static std::optional<stdio_message> read_message(std::istream& input) {
    std::string line;
    std::optional<std::size_t> content_length;

    while (std::getline(input, line)) {
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }

      const auto trimmed = trim(line);
      if (trimmed.empty()) {
        break;
      }

      if (trimmed.front() == '{' || trimmed.front() == '[') {
        return stdio_message {
          .body = std::move(line),
          .framing = message_framing::json_lines,
        };
      }

      const auto separator = line.find(':');
      if (separator == std::string::npos) {
        continue;
      }

      const auto name = trim(line.substr(0, separator));
      const auto value = trim(line.substr(separator + 1));
      if (equals_ascii_case(name, "Content-Length")) {
        content_length = parse_content_length(value);
      }
    }

    if (!input && !content_length) {
      return std::nullopt;
    }
    if (!content_length) {
      return std::nullopt;
    }

    std::string body(*content_length, '\0');
    input.read(body.data(), static_cast<std::streamsize>(body.size()));
    if (input.gcount() != static_cast<std::streamsize>(body.size())) {
      return std::nullopt;
    }
    return stdio_message {
      .body = std::move(body),
      .framing = message_framing::content_length,
    };
  }

  static std::optional<std::string> read_framed_message(std::istream& input) {
    auto message = read_message(input);
    if (!message || message->framing != message_framing::content_length) {
      return std::nullopt;
    }
    return std::move(message->body);
  }

private:
  template<typename WriteMessage>
  static void write_exchange(
    const mcp_server_exchange& exchange,
    WriteMessage&& write_message) {
    for (const auto& request : exchange.requests) {
      write_message(request);
    }
    for (const auto& notification : exchange.notifications) {
      write_message(notification);
    }
    if (exchange.response) {
      write_message(*exchange.response);
    }
  }

  static void configure_stdio_binary() noexcept {
#ifdef _WIN32
    (void)_setmode(_fileno(stdin), _O_BINARY);
    (void)_setmode(_fileno(stdout), _O_BINARY);
#endif
  }

  static std::string trim(std::string value) {
    const auto begin = value.find_first_not_of(" \t");
    if (begin == std::string::npos) {
      return {};
    }
    const auto end = value.find_last_not_of(" \t");
    return value.substr(begin, end - begin + 1);
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

  static std::optional<std::size_t> parse_content_length(const std::string& value) {
    if (value.empty()) {
      return std::nullopt;
    }

    std::size_t result = 0;
    for (const char ch : value) {
      if (ch < '0' || ch > '9') {
        return std::nullopt;
      }
      result = result * 10 + static_cast<std::size_t>(ch - '0');
    }
    return result;
  }
};

} // namespace wuwe::agent::mcp

#endif // WUWE_AGENT_MCP_STDIO_TRANSPORT_HPP
