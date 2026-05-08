#include <wuwe/agent/mcp/mcp_http_listener.hpp>

#include <chrono>
#include <stdexcept>
#include <utility>

#include <httplib/httplib.h>

namespace wuwe::agent::mcp {
namespace {

mcp_http_request to_mcp_http_request(const httplib::Request& request) {
  mcp_http_request output {
    .method = request.method,
    .body = request.body,
  };
  output.headers.reserve(request.headers.size());
  for (const auto& [name, value] : request.headers) {
    output.headers.push_back({ name, value });
  }
  return output;
}

void apply_headers(httplib::Response& target, const mcp_http_response& source) {
  for (const auto& [name, value] : source.headers) {
    target.set_header(name, value);
  }
}

void apply_cors(
  httplib::Response& response,
  const mcp_http_listener_options& options) {
  if (!options.enable_cors) {
    return;
  }
  response.set_header("Access-Control-Allow-Origin", options.cors_allow_origin);
  response.set_header("Access-Control-Allow-Methods", "POST, OPTIONS");
  response.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization");
}

} // namespace

mcp_http_listener::mcp_http_listener(
  mcp_server& server,
  mcp_http_listener_options options)
    : server_(server),
      options_(std::move(options)),
      listener_(std::make_unique<httplib::Server>()) {
  configure_routes();
}

mcp_http_listener::~mcp_http_listener() {
  stop();
}

bool mcp_http_listener::bind() {
  if (bound_port_ >= 0) {
    return true;
  }
  listener_->set_payload_max_length(options_.max_body_bytes);
  bound_port_ = options_.port == 0
      ? listener_->bind_to_any_port(options_.host)
      : listener_->bind_to_port(options_.host, options_.port) ? options_.port : -1;
  return bound_port_ >= 0;
}

bool mcp_http_listener::listen() {
  if (!bind()) {
    return false;
  }
  return listener_->listen_after_bind();
}

bool mcp_http_listener::start() {
  if (running()) {
    return true;
  }
  if (!bind()) {
    return false;
  }
  thread_ = std::thread([this] {
    listener_->listen_after_bind();
  });
  listener_->wait_until_ready();
  return listener_->is_running();
}

void mcp_http_listener::stop() {
  if (listener_) {
    listener_->stop();
  }
  if (thread_.joinable()) {
    thread_.join();
  }
}

bool mcp_http_listener::running() const {
  return listener_ && listener_->is_running();
}

int mcp_http_listener::bound_port() const noexcept {
  return bound_port_;
}

const mcp_http_listener_options& mcp_http_listener::options() const noexcept {
  return options_;
}

void mcp_http_listener::configure_routes() {
  if (options_.mcp_path.empty() || options_.mcp_path.front() != '/') {
    throw std::runtime_error("MCP HTTP listener mcp_path must start with '/'");
  }
  if (options_.health_path.empty() || options_.health_path.front() != '/') {
    throw std::runtime_error("MCP HTTP listener health_path must start with '/'");
  }

  listener_->Get(options_.health_path, [this](const httplib::Request&, httplib::Response& response) {
    response.status = running() ? 200 : 503;
    response.set_content(R"({"status":"ok"})", "application/json");
  });

  listener_->Options(options_.mcp_path, [this](const httplib::Request&, httplib::Response& response) {
    response.status = 204;
    apply_cors(response, options_);
  });

  listener_->Post(options_.mcp_path, [this](const httplib::Request& request, httplib::Response& response) {
    auto mcp_request = to_mcp_http_request(request);
    if (options_.authorize && !options_.authorize(mcp_request)) {
      response.status = 401;
      response.set_content(R"({"error":"unauthorized"})", "application/json");
      apply_cors(response, options_);
      return;
    }

    const auto mcp_response = transport_.handle(server_, mcp_request);
    response.status = mcp_response.status_code;
    apply_headers(response, mcp_response);
    apply_cors(response, options_);
    response.set_content(mcp_response.body, mcp_response.content_type);
  });
}

} // namespace wuwe::agent::mcp
