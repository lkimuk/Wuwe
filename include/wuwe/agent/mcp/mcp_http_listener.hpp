#ifndef WUWE_AGENT_MCP_HTTP_LISTENER_HPP
#define WUWE_AGENT_MCP_HTTP_LISTENER_HPP

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include <wuwe/agent/mcp/mcp_http_transport.hpp>

namespace httplib {
class Server;
}

namespace wuwe::agent::mcp {

struct mcp_http_listener_options {
  std::string host { "127.0.0.1" };
  int port { 0 };
  std::string mcp_path { "/mcp" };
  std::string health_path { "/healthz" };
  std::size_t max_body_bytes { 4 * 1024 * 1024 };
  bool enable_cors { false };
  std::string cors_allow_origin { "http://127.0.0.1" };
  std::function<bool(const mcp_http_request&)> authorize;
};

class mcp_http_listener {
public:
  explicit mcp_http_listener(mcp_server& server, mcp_http_listener_options options = {});
  ~mcp_http_listener();

  mcp_http_listener(const mcp_http_listener&) = delete;
  mcp_http_listener& operator=(const mcp_http_listener&) = delete;

  bool bind();
  bool listen();
  bool start();
  void stop();
  bool running() const;
  int bound_port() const noexcept;

  const mcp_http_listener_options& options() const noexcept;

private:
  void configure_routes();

  mcp_server& server_;
  mcp_http_listener_options options_;
  mcp_http_transport transport_;
  std::unique_ptr<httplib::Server> listener_;
  std::thread thread_;
  int bound_port_ { -1 };
};

} // namespace wuwe::agent::mcp

#endif // WUWE_AGENT_MCP_HTTP_LISTENER_HPP
