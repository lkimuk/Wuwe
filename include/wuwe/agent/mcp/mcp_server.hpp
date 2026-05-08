#ifndef WUWE_AGENT_MCP_SERVER_HPP
#define WUWE_AGENT_MCP_SERVER_HPP

#include <algorithm>
#include <cctype>
#include <functional>
#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <wuwe/agent/mcp/mcp_protocol.hpp>
#include <wuwe/agent/mcp/mcp_lifecycle.hpp>
#include <wuwe/agent/mcp/mcp_pagination.hpp>
#include <wuwe/agent/mcp/mcp_security.hpp>
#include <wuwe/agent/mcp/mcp_types.hpp>
#include <wuwe/agent/tools/tool.hpp>

namespace wuwe::agent::mcp {

class mcp_server {
public:
  explicit mcp_server(mcp_server_info info = {})
      : info_(std::move(info)) {
  }

  void add_tool(
    llm_tool tool,
    std::function<llm_tool_result(const std::string& arguments_json)> invoke) {
    add_mcp_tool(std::move(tool), [invoke = std::move(invoke)](const json& arguments) {
      return tool_call_result_from_llm(invoke(arguments.dump()));
    });
  }

  void add_mcp_tool(
    llm_tool tool,
    std::function<mcp_tool_call_result(const json& arguments)> invoke) {
    std::lock_guard lock(mutex_);
    if (tool.name.empty()) {
      return;
    }
    const auto name = tool.name;
    tools_[name] = mcp_tool_entry {
      .tool = std::move(tool),
      .invoke = std::move(invoke),
    };
  }

  template<typename ToolProvider>
  void add_tool_provider(ToolProvider& provider) {
    for (auto tool : provider.tools()) {
      const auto name = tool.name;
      add_tool(std::move(tool), [&provider, name](const std::string& arguments_json) {
        return provider.invoke(name, arguments_json);
      });
    }
  }

  void add_resource(
    mcp_resource resource,
    std::function<std::vector<mcp_resource_content>()> read) {
    std::lock_guard lock(mutex_);
    if (resource.uri.empty()) {
      return;
    }
    if (resource.name.empty()) {
      resource.name = resource.uri;
    }
    const auto uri = resource.uri;
    resources_[uri] = mcp_resource_entry {
      .resource = std::move(resource),
      .read = std::move(read),
    };
  }

  void add_resource_template(mcp_resource_template resource_template) {
    std::lock_guard lock(mutex_);
    if (resource_template.uri_template.empty()) {
      return;
    }
    if (resource_template.name.empty()) {
      resource_template.name = resource_template.uri_template;
    }
    resource_templates_[resource_template.uri_template] = std::move(resource_template);
  }

  void add_root(mcp_root root) {
    std::lock_guard lock(mutex_);
    if (root.uri.empty()) {
      return;
    }
    if (root.name.empty()) {
      root.name = root.uri;
    }
    roots_[root.uri] = std::move(root);
  }

  void set_roots(std::vector<mcp_root> roots) {
    std::lock_guard lock(mutex_);
    roots_.clear();
    for (auto& root : roots) {
      add_root(std::move(root));
    }
  }

  void add_prompt(
    mcp_prompt prompt,
    std::function<mcp_prompt_result(const json& arguments)> get) {
    std::lock_guard lock(mutex_);
    if (prompt.name.empty()) {
      return;
    }
    const auto name = prompt.name;
    prompts_[name] = mcp_prompt_entry {
      .prompt = std::move(prompt),
      .get = std::move(get),
    };
  }

  void set_access_policy(mcp_access_policy policy) {
    std::lock_guard lock(mutex_);
    access_policy_ = std::move(policy);
  }

  mcp_access_policy access_policy() const {
    std::lock_guard lock(mutex_);
    return access_policy_;
  }

  void set_audit_sink(std::function<void(const mcp_audit_event&)> sink) {
    std::lock_guard lock(mutex_);
    audit_sink_ = std::move(sink);
  }

  void set_auth_context(mcp_auth_context auth) {
    std::lock_guard lock(mutex_);
    auth_context_ = std::move(auth);
  }

  mcp_auth_context auth_context() const {
    std::lock_guard lock(mutex_);
    return auth_context_;
  }

  void set_list_page_size(std::size_t page_size) noexcept {
    std::lock_guard lock(mutex_);
    list_page_size_ = page_size;
  }

  void set_request_timeout(std::chrono::milliseconds timeout) noexcept {
    std::lock_guard lock(mutex_);
    request_timeout_ = timeout;
  }

  void emit_log(
    std::string level,
    std::string message,
    std::string logger = {},
    json data = json::object()) const {
    std::lock_guard lock(mutex_);
    json params {
      { "level", std::move(level) },
      { "data", std::move(data) },
    };
    if (!logger.empty()) {
      params["logger"] = std::move(logger);
    }
    if (!message.empty()) {
      params["data"]["message"] = std::move(message);
    }
    pending_notifications_.push_back(make_notification("notifications/message", std::move(params)));
  }

  void emit_progress(
    json progress_token,
    double progress,
    std::optional<double> total = std::nullopt,
    std::string message = {}) const {
    std::lock_guard lock(mutex_);
    json params {
      { "progressToken", std::move(progress_token) },
      { "progress", progress },
    };
    if (total) {
      params["total"] = *total;
    }
    if (!message.empty()) {
      params["message"] = std::move(message);
    }
    pending_notifications_.push_back(
      make_notification("notifications/progress", std::move(params)));
  }

  void emit_request_progress(
    json request_id,
    json progress_token,
    double progress,
    std::optional<double> total = std::nullopt,
    std::string message = {}) const {
    std::lock_guard lock(mutex_);
    request_registry_.progress(
      registry_key(request_id), progress_token, progress, total, message);
    emit_progress(std::move(progress_token), progress, total, std::move(message));
  }

  void emit_tool_list_changed() const {
    std::lock_guard lock(mutex_);
    pending_notifications_.push_back(make_notification("notifications/tools/list_changed"));
  }

  void emit_resource_list_changed() const {
    std::lock_guard lock(mutex_);
    pending_notifications_.push_back(make_notification("notifications/resources/list_changed"));
  }

  void emit_resource_updated(std::string uri) const {
    std::lock_guard lock(mutex_);
    if (!contains(subscribed_resource_uris_, uri)) {
      return;
    }
    pending_notifications_.push_back(make_notification(
      "notifications/resources/updated", { { "uri", std::move(uri) } }));
  }

  void emit_prompt_list_changed() const {
    std::lock_guard lock(mutex_);
    pending_notifications_.push_back(make_notification("notifications/prompts/list_changed"));
  }

  void emit_roots_list_changed() const {
    std::lock_guard lock(mutex_);
    pending_notifications_.push_back(make_notification("notifications/roots/list_changed"));
  }

  json request_sampling(json params) const {
    std::lock_guard lock(mutex_);
    if (!client_supports("sampling")) {
      return json(nullptr);
    }
    return enqueue_client_request("sampling/createMessage", std::move(params));
  }

  json request_sampling(const mcp_sampling_request& request) const {
    return request_sampling(request.to_json());
  }

  json request_elicitation(json params) const {
    std::lock_guard lock(mutex_);
    if (!client_supports("elicitation")) {
      return json(nullptr);
    }
    return enqueue_client_request("elicitation/create", std::move(params));
  }

  json request_elicitation(const mcp_elicitation_request& request) const {
    return request_elicitation(request.to_json());
  }

  bool is_cancelled(const json& request_id) const {
    std::lock_guard lock(mutex_);
    return contains(cancelled_request_ids_, request_id.dump());
  }

  bool is_resource_subscribed(const std::string& uri) const {
    std::lock_guard lock(mutex_);
    return contains(subscribed_resource_uris_, uri);
  }

  bool initialized() const {
    std::lock_guard lock(mutex_);
    return initialized_;
  }

  mcp_client_info client_info() const {
    std::lock_guard lock(mutex_);
    return client_info_;
  }

  json client_capabilities() const {
    std::lock_guard lock(mutex_);
    return client_capabilities_;
  }

  const mcp_request_registry& request_registry() const noexcept {
    return request_registry_;
  }

  std::vector<mcp_pending_client_request> pending_client_requests() const {
    std::lock_guard lock(mutex_);
    std::vector<mcp_pending_client_request> output;
    output.reserve(client_requests_.size());
    for (const auto& [_, request] : client_requests_) {
      output.push_back(request);
    }
    return output;
  }

  std::optional<mcp_pending_client_request> client_request(const json& id) const {
    std::lock_guard lock(mutex_);
    const auto it = client_requests_.find(registry_key(id));
    if (it == client_requests_.end()) {
      return std::nullopt;
    }
    return it->second;
  }

  void clear_completed_client_requests() const {
    std::lock_guard lock(mutex_);
    for (auto it = client_requests_.begin(); it != client_requests_.end();) {
      if (it->second.completed) {
        it = client_requests_.erase(it);
      }
      else {
        ++it;
      }
    }
  }

  std::vector<mcp_root> roots() const {
    std::lock_guard lock(mutex_);
    std::vector<mcp_root> output;
    output.reserve(roots_.size());
    for (const auto& [_, root] : roots_) {
      output.push_back(root);
    }
    return output;
  }

  std::vector<llm_tool> tools() const {
    std::lock_guard lock(mutex_);
    std::vector<llm_tool> output;
    output.reserve(tools_.size());
    for (const auto& [_, entry] : tools_) {
      output.push_back(entry.tool);
    }
    return output;
  }

  mcp_server_exchange handle_message_exchange(std::string_view message) const {
    std::lock_guard lock(mutex_);
    pending_client_request_messages_.clear();
    pending_notifications_.clear();
    auto response = handle_message(message);
    mcp_server_exchange exchange {
      .requests = std::move(pending_client_request_messages_),
      .notifications = std::move(pending_notifications_),
      .response = std::move(response),
    };
    pending_client_request_messages_.clear();
    pending_notifications_.clear();
    return exchange;
  }

  std::optional<std::string> handle_message(std::string_view message) const {
    std::lock_guard lock(mutex_);
    json request;
    try {
      request = json::parse(message);
    }
    catch (const std::exception& ex) {
      return make_error_response(nullptr,
        { .code = mcp_error_code::parse_error, .message = ex.what() });
    }

    if (request.is_array()) {
      if (request.empty()) {
        return make_error_response(nullptr,
          { .code = mcp_error_code::invalid_request, .message = "batch request must not be empty" });
      }

      json responses = json::array();
      for (const auto& item : request) {
        const auto response = handle_message(item.dump());
        if (response) {
          responses.push_back(json::parse(*response));
        }
      }
      if (responses.empty()) {
        return std::nullopt;
      }
      return responses.dump();
    }

    if (!request.is_object()) {
      return make_error_response(nullptr,
        { .code = mcp_error_code::invalid_request, .message = "request must be an object" });
    }

    if (request.value("jsonrpc", "") != "2.0") {
      return make_error_response(request.value("id", json(nullptr)),
        { .code = mcp_error_code::invalid_request, .message = "jsonrpc must be '2.0'" });
    }

    if (request.contains("id") && !request.contains("method") &&
        (request.contains("result") || request.contains("error"))) {
      record_client_response(request);
      return std::nullopt;
    }

    const bool has_id = request.contains("id");
    const json id = has_id ? request["id"] : json(nullptr);
    if (has_id && !is_valid_jsonrpc_id(id)) {
      return make_error_response(nullptr,
        { .code = mcp_error_code::invalid_request, .message = "id must be string, integer, or null" });
    }

    const auto method_it = request.find("method");
    if (method_it == request.end() || !method_it->is_string()) {
      if (!has_id) {
        return std::nullopt;
      }
      return make_error_response(id,
        { .code = mcp_error_code::invalid_request, .message = "method must be a string" });
    }

    const auto method = method_it->get<std::string>();
    if (!has_id) {
      handle_notification(method, request.value("params", json::object()));
      return std::nullopt;
    }

    if (method == "initialize") {
      return handle_initialize(id, request.value("params", json::object()));
    }
    if (method == "ping") {
      return make_success_response(id, json::object());
    }
    if (method == "tools/list") {
      return make_success_response(id, tools_list_result(request.value("params", json::object())));
    }
    if (method == "tools/call") {
      return handle_tools_call(id, request.value("params", json::object()));
    }
    if (method == "resources/list") {
      return make_success_response(id, resources_list_result(request.value("params", json::object())));
    }
    if (method == "resources/read") {
      return handle_resources_read(id, request.value("params", json::object()));
    }
    if (method == "resources/subscribe") {
      return handle_resources_subscribe(id, request.value("params", json::object()));
    }
    if (method == "resources/unsubscribe") {
      return handle_resources_unsubscribe(id, request.value("params", json::object()));
    }
    if (method == "resources/templates/list") {
      return make_success_response(id,
        resource_templates_list_result(request.value("params", json::object())));
    }
    if (method == "roots/list") {
      return make_success_response(id, roots_list_result(request.value("params", json::object())));
    }
    if (method == "prompts/list") {
      return make_success_response(id, prompts_list_result(request.value("params", json::object())));
    }
    if (method == "prompts/get") {
      return handle_prompts_get(id, request.value("params", json::object()));
    }

    return make_error_response(id,
      { .code = mcp_error_code::method_not_found, .message = "method not found: " + method });
  }

private:
  void handle_notification(const std::string& method, const json& params) const {
    if (method == "notifications/initialized") {
      initialized_ = true;
      return;
    }
    if (method == "notifications/cancelled" && params.is_object()) {
      const auto request_id = params.find("requestId");
      if (request_id != params.end()) {
        const auto key = request_id->dump();
        if (!contains(cancelled_request_ids_, key)) {
          cancelled_request_ids_.push_back(key);
        }
        const auto reason = params.value("reason", std::string {});
        request_registry_.cancel(key, reason);
      }
    }
  }

  json enqueue_client_request(std::string method, json params) const {
    const auto id = next_client_request_id_++;
    const json request_id = id;
    const auto message = make_request(request_id, method, params);
    client_requests_[registry_key(request_id)] = mcp_pending_client_request {
      .id = request_id,
      .method = std::move(method),
      .params = std::move(params),
    };
    pending_client_request_messages_.push_back(std::move(message));
    return request_id;
  }

  void record_client_response(const json& response) const {
    const auto id = response.find("id");
    if (id == response.end() || !is_valid_jsonrpc_id(*id)) {
      return;
    }
    const auto key = registry_key(*id);
    auto it = client_requests_.find(key);
    if (it == client_requests_.end()) {
      return;
    }
    it->second.completed = true;
    if (const auto result = response.find("result"); result != response.end()) {
      it->second.result = *result;
      it->second.error.reset();
      return;
    }
    const auto error = response.find("error");
    if (error == response.end() || !error->is_object()) {
      return;
    }
    mcp_error parsed_error;
    if (const auto code = error->find("code"); code != error->end() && code->is_number_integer()) {
      parsed_error.code = static_cast<mcp_error_code>(code->get<int>());
    }
    if (const auto message = error->find("message");
        message != error->end() && message->is_string()) {
      parsed_error.message = message->get<std::string>();
    }
    if (const auto data = error->find("data"); data != error->end()) {
      parsed_error.data = *data;
    }
    it->second.error = std::move(parsed_error);
  }

  std::optional<std::string> handle_initialize(const json& id, const json& params) const {
    if (!params.is_object()) {
      return make_error_response(id,
        { .code = mcp_error_code::invalid_params, .message = "params must be an object" });
    }

    const auto protocol_version = params.find("protocolVersion");
    if (protocol_version != params.end() && !protocol_version->is_string()) {
      return make_error_response(id,
        { .code = mcp_error_code::invalid_params,
          .message = "params.protocolVersion must be a string" });
    }

    const auto capabilities = params.find("capabilities");
    if (capabilities != params.end() && capabilities->is_object()) {
      client_capabilities_ = *capabilities;
    }
    else {
      client_capabilities_ = json::object();
    }
    client_capabilities_known_ = true;

    client_info_ = {};
    const auto client_info = params.find("clientInfo");
    if (client_info != params.end() && client_info->is_object()) {
      if (const auto name = client_info->find("name");
          name != client_info->end() && name->is_string()) {
        client_info_.name = name->get<std::string>();
      }
      if (const auto version = client_info->find("version");
          version != client_info->end() && version->is_string()) {
        client_info_.version = version->get<std::string>();
      }
    }

    return make_success_response(id, initialize_result());
  }

  json initialize_result() const {
    return {
      { "protocolVersion", std::string(default_protocol_version) },
      { "capabilities", {
          { "tools", { { "listChanged", true } } },
          { "resources", { { "subscribe", true }, { "listChanged", true } } },
          { "roots", { { "listChanged", true } } },
          { "prompts", { { "listChanged", true } } },
        } },
      { "serverInfo", {
          { "name", info_.name },
          { "version", info_.version },
        } },
    };
  }

  json tools_list_result(const json& params) const {
    json output = json::array();
    for (const auto& [_, entry] : tools_) {
      std::string reason;
      if (!access_allowed("tool", entry.tool.name, reason)) {
        continue;
      }
      output.push_back({
        { "name", entry.tool.name },
        { "description", entry.tool.description },
        { "inputSchema", parse_schema(entry.tool.parameters_json_schema) },
      });
    }
    return paginate_list_result("tools", std::move(output), params);
  }

  json resources_list_result(const json& params) const {
    json output = json::array();
    for (const auto& [_, entry] : resources_) {
      std::string reason;
      if (!access_allowed("resource", entry.resource.uri, reason)) {
        continue;
      }
      output.push_back(resource_to_json(entry.resource));
    }
    return paginate_list_result("resources", std::move(output), params);
  }

  json resource_templates_list_result(const json& params) const {
    json output = json::array();
    for (const auto& [_, resource_template] : resource_templates_) {
      std::string reason;
      if (!access_allowed("resource_template", resource_template.uri_template, reason)) {
        continue;
      }
      json item {
        { "uriTemplate", resource_template.uri_template },
        { "name", resource_template.name },
      };
      if (!resource_template.description.empty()) {
        item["description"] = resource_template.description;
      }
      if (!resource_template.mime_type.empty()) {
        item["mimeType"] = resource_template.mime_type;
      }
      output.push_back(std::move(item));
    }
    return paginate_list_result("resourceTemplates", std::move(output), params);
  }

  json roots_list_result(const json& params) const {
    json output = json::array();
    for (const auto& [_, root] : roots_) {
      json item {
        { "uri", root.uri },
      };
      if (!root.name.empty()) {
        item["name"] = root.name;
      }
      output.push_back(std::move(item));
    }
    return paginate_list_result("roots", std::move(output), params);
  }

  json prompts_list_result(const json& params) const {
    json output = json::array();
    for (const auto& [_, entry] : prompts_) {
      std::string reason;
      if (!access_allowed("prompt", entry.prompt.name, reason)) {
        continue;
      }
      json prompt {
        { "name", entry.prompt.name },
      };
      if (!entry.prompt.description.empty()) {
        prompt["description"] = entry.prompt.description;
      }

      json arguments = json::array();
      for (const auto& argument : entry.prompt.arguments) {
        json item {
          { "name", argument.name },
          { "required", argument.required },
        };
        if (!argument.description.empty()) {
          item["description"] = argument.description;
        }
        arguments.push_back(std::move(item));
      }
      if (!arguments.empty()) {
        prompt["arguments"] = std::move(arguments);
      }
      output.push_back(std::move(prompt));
    }
    return paginate_list_result("prompts", std::move(output), params);
  }

  std::optional<std::string> handle_tools_call(const json& id, const json& params) const {
    if (!params.is_object()) {
      return make_error_response(id,
        { .code = mcp_error_code::invalid_params, .message = "params must be an object" });
    }

    const auto name_it = params.find("name");
    if (name_it == params.end() || !name_it->is_string()) {
      return make_error_response(id,
        { .code = mcp_error_code::invalid_params, .message = "params.name must be a string" });
    }

    const auto name = name_it->get<std::string>();
    const auto tool_it = tools_.find(name);
    if (tool_it == tools_.end()) {
      return make_error_response(id,
        { .code = mcp_error_code::invalid_params, .message = "tool not found: " + name });
    }

    const auto arguments = params.value("arguments", json::object());
    if (!arguments.is_object()) {
      return make_error_response(id,
        { .code = mcp_error_code::invalid_params, .message = "params.arguments must be an object" });
    }

    std::string reason;
    if (!access_allowed("tool", name, reason)) {
      audit("tools/call", name, false, reason, arguments);
      return make_error_response(id,
        { .code = mcp_error_code::request_denied, .message = reason });
    }
    audit("tools/call", name, true, {}, arguments);

    const auto registry_id = registry_key(id);
    request_registry_.start(registry_id, "tools/call", name, arguments, request_timeout_);
    mcp_tool_call_result result;
    try {
      result = tool_it->second.invoke(arguments);
      if (request_registry_.timed_out(registry_id)) {
        request_registry_.fail(registry_id, "request timed out");
        return make_error_response(id,
          { .code = mcp_error_code::internal_error, .message = "request timed out" });
      }
      request_registry_.complete(registry_id);
    }
    catch (const std::exception& ex) {
      request_registry_.fail(registry_id, ex.what());
      return make_error_response(id,
        { .code = mcp_error_code::internal_error, .message = ex.what() });
    }

    return make_success_response(id, tool_call_result_to_json(result));
  }

  std::optional<std::string> handle_resources_read(const json& id, const json& params) const {
    if (!params.is_object()) {
      return make_error_response(id,
        { .code = mcp_error_code::invalid_params, .message = "params must be an object" });
    }

    const auto uri_it = params.find("uri");
    if (uri_it == params.end() || !uri_it->is_string()) {
      return make_error_response(id,
        { .code = mcp_error_code::invalid_params, .message = "params.uri must be a string" });
    }

    const auto uri = uri_it->get<std::string>();
    const auto resource_it = resources_.find(uri);
    if (resource_it == resources_.end()) {
      return make_error_response(id,
        { .code = mcp_error_code::invalid_params, .message = "resource not found: " + uri });
    }

    std::string reason;
    if (!access_allowed("resource", uri, reason)) {
      audit("resources/read", uri, false, reason, params);
      return make_error_response(id,
        { .code = mcp_error_code::request_denied, .message = reason });
    }
    audit("resources/read", uri, true, {}, params);

    const auto registry_id = registry_key(id);
    request_registry_.start(registry_id, "resources/read", uri, params, request_timeout_);
    std::vector<mcp_resource_content> contents;
    try {
      contents = resource_it->second.read();
      if (request_registry_.timed_out(registry_id)) {
        request_registry_.fail(registry_id, "request timed out");
        return make_error_response(id,
          { .code = mcp_error_code::internal_error, .message = "request timed out" });
      }
      request_registry_.complete(registry_id);
    }
    catch (const std::exception& ex) {
      request_registry_.fail(registry_id, ex.what());
      return make_error_response(id,
        { .code = mcp_error_code::internal_error, .message = ex.what() });
    }

    json output = json::array();
    for (const auto& content : contents) {
      output.push_back(resource_content_to_json(content));
    }
    return make_success_response(id, { { "contents", std::move(output) } });
  }

  std::optional<std::string> handle_resources_subscribe(
    const json& id,
    const json& params) const {
    const auto uri = resource_uri_param(id, params);
    if (!uri) {
      return make_error_response(id,
        { .code = mcp_error_code::invalid_params, .message = "params.uri must be a string" });
    }

    const auto resource_it = resources_.find(*uri);
    if (resource_it == resources_.end()) {
      return make_error_response(id,
        { .code = mcp_error_code::invalid_params, .message = "resource not found: " + *uri });
    }

    std::string reason;
    if (!access_allowed("resource", *uri, reason)) {
      return make_error_response(id,
        { .code = mcp_error_code::request_denied, .message = reason });
    }

    if (!contains(subscribed_resource_uris_, *uri)) {
      subscribed_resource_uris_.push_back(*uri);
    }
    return make_success_response(id, json::object());
  }

  std::optional<std::string> handle_resources_unsubscribe(
    const json& id,
    const json& params) const {
    const auto uri = resource_uri_param(id, params);
    if (!uri) {
      return make_error_response(id,
        { .code = mcp_error_code::invalid_params, .message = "params.uri must be a string" });
    }

    for (auto it = subscribed_resource_uris_.begin();
         it != subscribed_resource_uris_.end();
         ++it) {
      if (*it == *uri) {
        subscribed_resource_uris_.erase(it);
        break;
      }
    }
    return make_success_response(id, json::object());
  }

  std::optional<std::string> handle_prompts_get(const json& id, const json& params) const {
    if (!params.is_object()) {
      return make_error_response(id,
        { .code = mcp_error_code::invalid_params, .message = "params must be an object" });
    }

    const auto name_it = params.find("name");
    if (name_it == params.end() || !name_it->is_string()) {
      return make_error_response(id,
        { .code = mcp_error_code::invalid_params, .message = "params.name must be a string" });
    }

    const auto name = name_it->get<std::string>();
    const auto prompt_it = prompts_.find(name);
    if (prompt_it == prompts_.end()) {
      return make_error_response(id,
        { .code = mcp_error_code::invalid_params, .message = "prompt not found: " + name });
    }

    const auto arguments = params.value("arguments", json::object());
    if (!arguments.is_object()) {
      return make_error_response(id,
        { .code = mcp_error_code::invalid_params, .message = "params.arguments must be an object" });
    }

    std::string reason;
    if (!access_allowed("prompt", name, reason)) {
      audit("prompts/get", name, false, reason, arguments);
      return make_error_response(id,
        { .code = mcp_error_code::request_denied, .message = reason });
    }
    audit("prompts/get", name, true, {}, arguments);

    const auto registry_id = registry_key(id);
    request_registry_.start(registry_id, "prompts/get", name, arguments, request_timeout_);
    mcp_prompt_result prompt_result;
    try {
      prompt_result = prompt_it->second.get(arguments);
      if (request_registry_.timed_out(registry_id)) {
        request_registry_.fail(registry_id, "request timed out");
        return make_error_response(id,
          { .code = mcp_error_code::internal_error, .message = "request timed out" });
      }
      request_registry_.complete(registry_id);
    }
    catch (const std::exception& ex) {
      request_registry_.fail(registry_id, ex.what());
      return make_error_response(id,
        { .code = mcp_error_code::internal_error, .message = ex.what() });
    }

    return make_success_response(id, prompt_result_to_json(prompt_result));
  }

  static json parse_schema(const std::string& schema) {
    if (schema.empty()) {
      return json::object();
    }
    try {
      return json::parse(schema);
    }
    catch (...) {
      return json {
        { "type", "object" },
        { "description", schema },
      };
    }
  }

  static mcp_tool_call_result tool_call_result_from_llm(const llm_tool_result& result) {
    return {
      .content = {
        {
          .type = "text",
          .text = result.content,
        },
      },
      .is_error = static_cast<bool>(result.error_code),
    };
  }

  static json tool_call_result_to_json(const mcp_tool_call_result& result) {
    json content = json::array();
    for (const auto& item : result.content) {
      content.push_back(content_to_json(item));
    }
    return {
      { "content", std::move(content) },
      { "isError", result.is_error },
    };
  }

  static json resource_to_json(const mcp_resource& resource) {
    json output {
      { "uri", resource.uri },
      { "name", resource.name },
    };
    if (!resource.description.empty()) {
      output["description"] = resource.description;
    }
    if (!resource.mime_type.empty()) {
      output["mimeType"] = resource.mime_type;
    }
    return output;
  }

  static json resource_content_to_json(const mcp_resource_content& content) {
    json output {
      { "uri", content.uri },
    };
    if (!content.blob.empty()) {
      output["blob"] = content.blob;
    }
    else {
      output["text"] = content.text;
    }
    if (!content.mime_type.empty()) {
      output["mimeType"] = content.mime_type;
    }
    return output;
  }

  static json prompt_result_to_json(const mcp_prompt_result& prompt_result) {
    json output {
      { "messages", json::array() },
    };
    if (!prompt_result.description.empty()) {
      output["description"] = prompt_result.description;
    }
    for (const auto& message : prompt_result.messages) {
      const auto content = message.text.empty()
                             ? content_to_json(message.content)
                             : content_to_json({ .type = "text", .text = message.text });
      output["messages"].push_back({
        { "role", message.role },
        { "content", std::move(content) },
      });
    }
    return output;
  }

  static json content_to_json(const mcp_content& content) {
    json output {
      { "type", content.type.empty() ? "text" : content.type },
    };
    if (!content.text.empty()) {
      output["text"] = content.text;
    }
    if (!content.data.empty()) {
      output["data"] = content.data;
    }
    if (!content.mime_type.empty()) {
      output["mimeType"] = content.mime_type;
    }
    if (!content.uri.empty()) {
      output["uri"] = content.uri;
    }
    return output;
  }

  json paginate_list_result(std::string_view key, json items, const json& params) const {
    return mcp_list_paginator(list_page_size_).page(key, std::move(items), params);
  }

  static std::optional<std::string> resource_uri_param(const json&, const json& params) {
    if (!params.is_object()) {
      return std::nullopt;
    }
    const auto uri = params.find("uri");
    if (uri == params.end() || !uri->is_string()) {
      return std::nullopt;
    }
    return uri->get<std::string>();
  }

  static std::string registry_key(const json& id) {
    return id.dump();
  }

  bool client_supports(std::string_view capability) const {
    if (!client_capabilities_known_) {
      return true;
    }
    const auto found = client_capabilities_.find(std::string(capability));
    return found != client_capabilities_.end() && found->is_object();
  }

  bool access_allowed(
    std::string_view kind,
    const std::string& target,
    std::string& reason) const {
    const auto& allowed = allowed_targets(kind);
    const auto& denied = denied_targets(kind);
    if (contains(denied, target)) {
      reason = "access denied for " + std::string(kind) + ": " + target;
      return false;
    }
    if (!allowed.empty() && !contains(allowed, target)) {
      reason = "access not allowed for " + std::string(kind) + ": " + target;
      return false;
    }
    if (!access_policy_.allow_unlisted && allowed.empty()) {
      reason = "access requires explicit allowlist for " + std::string(kind) + ": " + target;
      return false;
    }
    return true;
  }

  const std::vector<std::string>& allowed_targets(std::string_view kind) const {
    if (kind == "tool") {
      return access_policy_.allowed_tools;
    }
    if (kind == "resource") {
      return access_policy_.allowed_resources;
    }
    if (kind == "resource_template") {
      return access_policy_.allowed_resource_templates;
    }
    return access_policy_.allowed_prompts;
  }

  const std::vector<std::string>& denied_targets(std::string_view kind) const {
    if (kind == "tool") {
      return access_policy_.denied_tools;
    }
    if (kind == "resource") {
      return access_policy_.denied_resources;
    }
    if (kind == "resource_template") {
      return access_policy_.denied_resource_templates;
    }
    return access_policy_.denied_prompts;
  }

  static bool contains(const std::vector<std::string>& values, const std::string& value) {
    for (const auto& item : values) {
      if (item == value) {
        return true;
      }
    }
    return false;
  }

  void audit(
    std::string action,
    std::string target,
    bool allowed,
    std::string reason,
    json arguments) const {
    if (!audit_sink_) {
      return;
    }
    auto redacted = false;
    auto sanitized_arguments = redact_arguments(std::move(arguments), redacted);
    audit_sink_(mcp_audit_event {
      .action = std::move(action),
      .target = std::move(target),
      .auth = auth_context_,
      .tenant_id = access_policy_.tenant_id,
      .scopes = access_policy_.scopes,
      .allowed = allowed,
      .reason = std::move(reason),
      .arguments = std::move(sanitized_arguments),
      .redacted = redacted,
    });
  }

  json redact_arguments(json value, bool& redacted) const {
    if (value.is_object()) {
      for (auto& item : value.items()) {
        if (should_redact_argument_key(item.key())) {
          item.value() = "[redacted]";
          redacted = true;
        }
        else {
          item.value() = redact_arguments(std::move(item.value()), redacted);
        }
      }
      return value;
    }
    if (value.is_array()) {
      for (auto& item : value) {
        item = redact_arguments(std::move(item), redacted);
      }
    }
    return value;
  }

  bool should_redact_argument_key(std::string key) const {
    key = lowercase(std::move(key));
    for (const auto& redacted_key : access_policy_.redacted_argument_keys) {
      if (key == lowercase(redacted_key)) {
        return true;
      }
    }
    return false;
  }

  static std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
      return static_cast<char>(std::tolower(character));
    });
    return value;
  }

  mcp_server_info info_;
  std::map<std::string, mcp_tool_entry> tools_;
  std::map<std::string, mcp_resource_entry> resources_;
  std::map<std::string, mcp_resource_template> resource_templates_;
  std::map<std::string, mcp_root> roots_;
  std::map<std::string, mcp_prompt_entry> prompts_;
  mcp_access_policy access_policy_;
  mcp_auth_context auth_context_;
  std::function<void(const mcp_audit_event&)> audit_sink_;
  mutable std::vector<std::string> pending_client_request_messages_;
  mutable std::vector<std::string> pending_notifications_;
  mutable std::map<std::string, mcp_pending_client_request> client_requests_;
  mutable std::vector<std::string> cancelled_request_ids_;
  mutable std::vector<std::string> subscribed_resource_uris_;
  std::size_t list_page_size_ { 0 };
  mutable bool initialized_ { false };
  mutable mcp_client_info client_info_;
  mutable json client_capabilities_ { json::object() };
  mutable bool client_capabilities_known_ { false };
  mutable mcp_request_registry request_registry_;
  std::chrono::milliseconds request_timeout_ { 0 };
  mutable int next_client_request_id_ { 1 };
  mutable std::recursive_mutex mutex_;
};

} // namespace wuwe::agent::mcp

#endif // WUWE_AGENT_MCP_SERVER_HPP
