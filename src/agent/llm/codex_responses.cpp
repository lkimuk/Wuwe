#include "codex_responses.h"
#include "../auth/codex_oauth_protocol.h"

#include <algorithm>
#include <limits>

WUWE_NAMESPACE_BEGIN
namespace codex_responses {
namespace {
using agent::llm_error_code;
using codex_detail::parse;
using codex_detail::safe_text;
using codex_detail::string_field;

bool function_name(const std::string& name) {
  return !name.empty() && name.size() <= 64 &&
         std::all_of(name.begin(), name.end(), [](unsigned char c) {
           return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '_' || c == '-';
         });
}
std::optional<int> integer(const json& value, std::size_t maximum) {
  if (!value.is_number_integer() ||
      (value.is_number_integer() && !value.is_number_unsigned() && value.get<std::int64_t>() < 0))
    return {};
  const auto n = value.get<std::uint64_t>();
  if (n > maximum || n > static_cast<std::uint64_t>((std::numeric_limits<int>::max)()))
    return {};
  return static_cast<int>(n);
}
bool reasoning_item(const json& item) {
  if (!item.is_object() || string_field(item, "type") != "reasoning" ||
      (item.contains("id") && !item["id"].is_null() &&
        !safe_text(string_field(item, "id"), 1024)) ||
      !item.contains("summary") || !item["summary"].is_array())
    return false;
  if (item.contains("encrypted_content") && !item["encrypted_content"].is_null() &&
      !item["encrypted_content"].is_string())
    return false;
  for (const auto& summary : item["summary"])
    if (!summary.is_object() || string_field(summary, "type") != "summary_text" ||
        !summary.contains("text") || !summary["text"].is_string())
      return false;
  return true;
}
json replay_reasoning(const json& item) {
  // Deliberately exclude unrelated server metadata from subsequent requests.
  json result { { "type", "reasoning" }, { "summary", item["summary"] } };
  if (item.contains("id") && item["id"].is_string())
    result["id"] = item["id"];
  if (item.contains("encrypted_content") && item["encrypted_content"].is_string())
    result["encrypted_content"] = item["encrypted_content"];
  return result;
}
json replay_item(const json& item) {
  const auto type = string_field(item, "type");
  if (item.contains("id") && !item["id"].is_null() &&
      (!item["id"].is_string() || !safe_text(string_field(item, "id"), 1024)))
    return {};
  if (type == "reasoning")
    return reasoning_item(item) ? replay_reasoning(item) : json();
  json result { { "type", type } };
  if (item.contains("id") && item["id"].is_string())
    result["id"] = item["id"];
  if (type == "function_call") {
    if (!safe_text(string_field(item, "call_id"), 1024) ||
        !function_name(string_field(item, "name")) ||
        !parse(string_field(item, "arguments")).is_object())
      return {};
    for (const auto* field : { "call_id", "name", "arguments" })
      result[field] = item[field];
    return result;
  }
  if (type != "message" || string_field(item, "role") != "assistant" || !item.contains("content") ||
      !item["content"].is_array())
    return {};
  result["role"] = "assistant";
  result["content"] = json::array();
  for (const auto& part : item["content"]) {
    const auto kind = string_field(part, "type");
    const char* field = kind == "refusal" ? "refusal" : "text";
    if ((kind != "output_text" && kind != "refusal") || !part.contains(field) ||
        !part[field].is_string())
      return {};
    result["content"].push_back({ { "type", "output_text" }, { "text", part[field] } });
  }
  if (item.contains("phase") && !item["phase"].is_null()) {
    const auto phase = string_field(item, "phase");
    if (phase != "commentary" && phase != "final_answer")
      return {};
    result["phase"] = phase;
  }
  return result;
}
llm_error_code server_error(const json& error) {
  const auto code = string_field(error, "code");
  if (code == "rate_limit_exceeded" || code == "insufficient_quota")
    return llm_error_code::rate_limited;
  if (code == "context_length_exceeded")
    return llm_error_code::context_budget_exceeded;
  if (code == "model_not_found")
    return llm_error_code::model_unavailable;
  if (code == "invalid_api_key" || code == "authentication_error")
    return llm_error_code::authentication_failed;
  return llm_error_code::api_error;
}
} // namespace

payload_result build_payload(
  const llm_request& request, const codex_llm_options& options, const oauth_account_info& account) {
  const auto fail = [](llm_error_code error, std::string unsupported = {}) {
    return payload_result { .error = error, .unsupported = std::move(unsupported) };
  };
  // Bound strings and parse schemas with a nesting limit before generic schema validation.
  std::size_t bytes = 0;
  const auto count = [&](std::size_t size) {
    if (size > options.max_request_bytes - bytes)
      return false;
    bytes += size;
    return true;
  };
  for (const auto& message : request.messages) {
    if (!count(message.content.size()) || !count(message.reasoning_content.size()) ||
        (message.provider_state && !count(message.provider_state->data.size())))
      return fail(llm_error_code::invalid_request);
    for (const auto& call : message.tool_calls)
      if (!count(call.arguments_json.size()))
        return fail(llm_error_code::invalid_request);
  }
  for (const auto& tool : request.tools) {
    if (!function_name(tool.name) || !count(tool.description.size()) ||
        !count(tool.parameters_json_schema.size()) ||
        !parse(tool.parameters_json_schema).is_object())
      return fail(llm_error_code::invalid_request);
  }
  llm_provider_capabilities caps { .streaming = true,
    .tools = true,
    .tool_choice = true,
    .reasoning_summary = true,
    .streaming_reasoning_summary = true,
    .json_schema_output = true,
    .provider_state = true };
  if (const auto valid = agent::llm::validate_llm_request(request, caps); !valid)
    return { .error = valid.error_code, .unsupported = valid.capability };
  if (request.max_output_tokens)
    return fail(llm_error_code::unsupported_capability, "max_output_tokens");
  // llm_request has a legacy non-optional temperature; its default means unspecified here.
  if (request.temperature != llm_request {}.temperature)
    return fail(llm_error_code::unsupported_capability, "temperature");
  if (request.thinking_mode != llm_thinking_mode::provider_default)
    return fail(llm_error_code::unsupported_capability, "thinking_mode");
  const auto model = request.model.empty() ? options.model : request.model;
  if (!safe_text(model, 256) || request.messages.empty() ||
      (!request.provider.empty() && request.provider != codex_detail::provider))
    return fail(llm_error_code::invalid_request);
  json body { { "model", model },
    { "instructions", "" },
    { "input", json::array() },
    { "tools", json::array() },
    { "tool_choice", "auto" },
    { "parallel_tool_calls", options.parallel_tool_calls },
    { "store", false },
    { "stream", true },
    { "include", json::array({ "reasoning.encrypted_content" }) } };
  std::string instructions;
  std::map<std::string, std::string> pending;
  std::set<std::string> seen;
  for (const auto& message : request.messages) {
    if ((message.name && !message.name->empty() && message.role != "tool") ||
        (message.role != "assistant" && (!message.tool_calls.empty() || message.provider_state)))
      return fail(llm_error_code::invalid_request);
    if (message.role == "system" || message.role == "developer") {
      if (message.tool_call_id)
        return fail(llm_error_code::invalid_request);
      if (!instructions.empty())
        instructions += "\n\n";
      instructions += message.content;
      continue;
    }
    if (message.role == "tool") {
      if (!message.tool_call_id)
        return fail(llm_error_code::invalid_request);
      const auto pending_call = pending.find(*message.tool_call_id);
      if (pending_call == pending.end() || (message.name && *message.name != pending_call->second))
        return fail(llm_error_code::invalid_request);
      pending.erase(pending_call);
      body["input"].push_back({ { "type", "function_call_output" },
        { "call_id", *message.tool_call_id },
        { "output", message.content } });
      continue;
    }
    if ((message.role != "user" && message.role != "assistant") || message.tool_call_id ||
        !pending.empty())
      return fail(llm_error_code::invalid_request);
    if (message.provider_state) {
      const auto& state = *message.provider_state;
      const auto decoded = parse(state.data);
      if (state.provider != codex_detail::provider || !decoded.is_object() ||
          decoded.value("version", json()) != 1 ||
          string_field(decoded, "account_id") != account.key.account_id ||
          string_field(decoded, "subject_id") != account.subject_id ||
          string_field(decoded, "workspace") != account.upstream_account_id ||
          string_field(decoded, "model") != model || !decoded.contains("items") ||
          !decoded["items"].is_array() || decoded["items"].size() > options.max_output_items)
        return fail(llm_error_code::invalid_request);
      std::string replay_text;
      std::size_t call_index = 0;
      for (const auto& item : decoded["items"]) {
        auto replay = replay_item(item);
        if (replay.is_null())
          return fail(llm_error_code::invalid_request);
        const auto type = string_field(replay, "type");
        if (type == "message") {
          for (const auto& part : replay["content"])
            replay_text += part["text"].get<std::string>();
        }
        else if (type == "function_call") {
          if (call_index >= message.tool_calls.size())
            return fail(llm_error_code::invalid_request);
          const auto& call = message.tool_calls[call_index++];
          if (call.id != string_field(replay, "call_id") ||
              call.name != string_field(replay, "name") ||
              parse(call.arguments_json) != parse(replay["arguments"].get<std::string>()) ||
              !seen.insert(call.id).second)
            return fail(llm_error_code::invalid_request);
          pending.emplace(call.id, call.name);
        }
        body["input"].push_back(std::move(replay));
      }
      if (replay_text != message.content || call_index != message.tool_calls.size())
        return fail(llm_error_code::invalid_request);
      continue;
    }
    if (!message.content.empty() || (message.role == "user" && message.tool_calls.empty())) {
      body["input"].push_back({ { "type", "message" },
        { "role", message.role },
        { "content",
          json::array({ { { "type", message.role == "user" ? "input_text" : "output_text" },
            { "text", message.content } } }) } });
    }
    for (const auto& call : message.tool_calls) {
      if (!safe_text(call.id, 1024) || !function_name(call.name) ||
          !parse(call.arguments_json).is_object() || !seen.insert(call.id).second)
        return fail(llm_error_code::invalid_request);
      pending.emplace(call.id, call.name);
      body["input"].push_back({ { "type", "function_call" },
        { "call_id", call.id },
        { "name", call.name },
        { "arguments", call.arguments_json } });
    }
  }
  if (!pending.empty() || body["input"].empty())
    return fail(llm_error_code::invalid_request);
  const auto language = llm_language_contract(request.language);
  if (!language.empty()) {
    if (!instructions.empty())
      instructions += "\n\n";
    instructions += language;
  }
  body["instructions"] = std::move(instructions);
  for (const auto& tool : request.tools)
    body["tools"].push_back({ { "type", "function" },
      { "name", tool.name },
      { "description", tool.description },
      { "parameters", parse(tool.parameters_json_schema) },
      { "strict", false } });
  if (request.tool_choice) {
    switch (request.tool_choice->mode) {
      case llm_tool_choice_mode::auto_:
        break;
      case llm_tool_choice_mode::none:
        body["tool_choice"] = "none";
        break;
      case llm_tool_choice_mode::required:
        body["tool_choice"] = "required";
        break;
      case llm_tool_choice_mode::named:
        body["tool_choice"] = { { "type", "function" }, { "name", request.tool_choice->name } };
        break;
    }
  }
  if (options.reasoning_effort)
    body["reasoning"]["effort"] = *options.reasoning_effort;
  if (options.reasoning_summary)
    body["reasoning"]["summary"] = "auto";
  if (request.json_schema_output) {
    const auto& output = *request.json_schema_output;
    body["text"]["format"] = { { "type", "json_schema" },
      { "name", output.name },
      { "schema", output.schema },
      { "strict", output.strict } };
  }
  return { std::move(body), {}, {} };
}

decoder::decoder(const codex_llm_options& options, const llm_request& request,
  const oauth_account_info& account, std::string model, std::function<void(llm_stream_event)> emit)
    : options_(options), request_(request), account_(account), model_(std::move(model)),
      emit_(std::move(emit)) {
}
bool decoder::fail(llm_error_code error) {
  result.error_code = error;
  return false;
}
decoder::item_state* decoder::item(int index, const std::string& id, const std::string& type) {
  if (index < 0 || static_cast<std::size_t>(index) >= options_.max_output_items ||
      (!id.empty() && !safe_text(id, 1024))) {
    fail();
    return nullptr;
  }
  auto& state = items_[index];
  if ((!state.id.empty() && !id.empty() && state.id != id) ||
      (!state.type.empty() && state.type != type)) {
    fail();
    return nullptr;
  }
  if (!id.empty()) {
    const auto [at, inserted] = ids_.emplace(id, index);
    if (!inserted && at->second != index) {
      fail();
      return nullptr;
    }
    state.id = id;
  }
  state.type = type;
  return &state;
}
bool decoder::finalize_item(int index, const json& value) {
  if (!value.is_object())
    return fail();
  const auto type = string_field(value, "type");
  const auto id = string_field(value, "id");
  if ((value.contains("id") && !value["id"].is_null() && !safe_text(id, 1024)) ||
      (value.contains("status") && !value["status"].is_null() &&
        string_field(value, "status") != "completed"))
    return fail();
  if (type != "message" && type != "function_call" && type != "reasoning")
    return fail(llm_error_code::unsupported_capability);
  auto* state = item(index, id, type);
  if (!state)
    return false;
  if (state->done)
    return state->final_item == value || fail();
  const auto reconcile = [&](std::string& previous, const std::string& text, bool summary) {
    if (!text.starts_with(previous))
      return fail();
    auto suffix = text.substr(previous.size());
    previous = text;
    if (!suffix.empty()) {
      if (summary) {
        result.reasoning_summary += suffix;
        emit_(
          { .type = llm_stream_event_type::reasoning_delta, .reasoning_delta = std::move(suffix) });
      }
      else {
        result.content += suffix;
        emit_({ .type = llm_stream_event_type::content_delta, .content_delta = std::move(suffix) });
      }
    }
    return true;
  };
  if (type == "function_call") {
    llm_tool_call call {
      string_field(value, "call_id"), string_field(value, "name"), string_field(value, "arguments")
    };
    if (!safe_text(call.id, 1024) || !function_name(call.name) ||
        (!state->call.id.empty() && state->call.id != call.id) ||
        (!state->call.name.empty() && state->call.name != call.name) ||
        (state->arguments_seen && !call.arguments_json.starts_with(state->call.arguments_json)))
      return fail();
    if (!parse(call.arguments_json).is_object())
      return fail(llm_error_code::invalid_tool_arguments);
    if (std::none_of(request_.tools.begin(), request_.tools.end(), [&](const auto& t) {
          return t.name == call.name;
        }))
      return fail();
    const auto suffix = call.arguments_json.substr(state->call.arguments_json.size());
    if (!suffix.empty() || state->call.id.empty())
      emit_({ .type = llm_stream_event_type::tool_call_delta,
        .tool_call_delta = llm_tool_call_delta { index,
          state->call.id.empty() ? call.id : "",
          state->call.name.empty() ? call.name : "",
          suffix } });
    state->call = std::move(call);
  }
  else if (type == "message") {
    if (replay_item(value).is_null())
      return fail();
    int part = 0;
    for (const auto& content : value["content"]) {
      if (static_cast<std::size_t>(part) >= options_.max_output_items)
        return fail();
      const auto kind = string_field(content, "type");
      const char* field = kind == "refusal" ? "refusal" : "text";
      if ((kind != "output_text" && kind != "refusal") || !content.contains(field) ||
          !content[field].is_string())
        return fail();
      if (!reconcile(state->text[part++], content[field].get<std::string>(), false))
        return false;
      if (kind == "refusal")
        result.finish_reason = "refusal";
    }
    if (state->text.size() != value["content"].size())
      return fail();
  }
  else {
    if (!reasoning_item(value))
      return fail();
    int part = 0;
    for (const auto& summary : value["summary"]) {
      if (static_cast<std::size_t>(part) >= options_.max_output_items ||
          !reconcile(state->summary[part++], summary["text"].get<std::string>(), true))
        return false;
    }
    if (state->summary.size() != value["summary"].size())
      return fail();
  }
  state->done = true;
  state->final_item = value;
  return true;
}

bool decoder::complete(const json& response) {
  if (!response.is_object() ||
      (response.contains("status") && string_field(response, "status") != "completed"))
    return fail();
  if (response.contains("output")) {
    const auto& output = response["output"];
    if (!output.is_array() || output.size() > options_.max_output_items)
      return fail();
    for (std::size_t n = 0; n < output.size(); ++n)
      if (!finalize_item(static_cast<int>(n), output[n]))
        return false;
    if (items_.size() != output.size())
      return fail();
  }
  std::set<std::string> call_ids;
  for (const auto& message : request_.messages)
    for (const auto& call : message.tool_calls)
      call_ids.insert(call.id);
  json replay = json::array();
  bool stateful = items_.size() > 1;
  for (const auto& [index, state] : items_) {
    if (!state.done)
      return fail();
    if (state.type == "function_call") {
      if (!call_ids.insert(state.call.id).second)
        return fail();
      result.tool_calls.push_back(state.call);
    }
    // Preserve native order and assistant phase, excluding hidden reasoning plaintext.
    if (state.type == "reasoning" && !string_field(state.final_item, "encrypted_content").empty())
      stateful = true;
    if (state.type == "message" && state.final_item.contains("phase") &&
        !state.final_item["phase"].is_null())
      stateful = true;
    if (state.type != "reasoning" || !string_field(state.final_item, "encrypted_content").empty())
      replay.push_back(replay_item(state.final_item));
  }
  if (stateful && !replay.empty()) {
    result.provider_state = llm_provider_state { codex_detail::provider,
      json { { "version", 1 },
        { "account_id", account_.key.account_id },
        { "subject_id", account_.subject_id },
        { "workspace", account_.upstream_account_id },
        { "model", model_ },
        { "items", std::move(replay) } }
        .dump() };
  }
  if (request_.tool_choice) {
    const auto& choice = *request_.tool_choice;
    if ((choice.mode == llm_tool_choice_mode::none && !result.tool_calls.empty()) ||
        ((choice.mode == llm_tool_choice_mode::required ||
           choice.mode == llm_tool_choice_mode::named) &&
          result.tool_calls.empty()) ||
        (choice.mode == llm_tool_choice_mode::named &&
          std::any_of(result.tool_calls.begin(), result.tool_calls.end(), [&](const auto& call) {
            return call.name != choice.name;
          })))
      return fail();
  }
  if (response.contains("usage") && !response["usage"].is_null()) {
    const auto& usage = response["usage"];
    const auto number = [&](const json& source, const char* key, int& target) {
      if (!source.is_object())
        return false;
      if (!source.contains(key))
        return true;
      const auto n = integer(source[key], (std::numeric_limits<int>::max)());
      if (!n)
        return false;
      target = *n;
      return true;
    };
    if (!number(usage, "input_tokens", result.usage.prompt_tokens) ||
        !number(usage, "output_tokens", result.usage.completion_tokens))
      return fail();
    if (result.usage.prompt_tokens >
        (std::numeric_limits<int>::max)() - result.usage.completion_tokens)
      return fail();
    result.usage.total_tokens = result.usage.prompt_tokens + result.usage.completion_tokens;
    if (!number(usage, "total_tokens", result.usage.total_tokens))
      return fail();
    if (usage.contains("input_tokens_details") && !usage["input_tokens_details"].is_null() &&
        !number(usage["input_tokens_details"], "cached_tokens", result.usage.cached_prompt_tokens))
      return fail();
    if (usage.contains("output_tokens_details") && !usage["output_tokens_details"].is_null() &&
        !number(usage["output_tokens_details"], "reasoning_tokens", result.usage.reasoning_tokens))
      return fail();
    if (result.usage.cached_prompt_tokens > result.usage.prompt_tokens ||
        result.usage.reasoning_tokens > result.usage.completion_tokens)
      return fail();
  }
  if (result.content.empty() && result.tool_calls.empty() && result.reasoning_summary.empty())
    return fail(llm_error_code::empty_response);
  if (result.finish_reason.empty())
    result.finish_reason = result.tool_calls.empty() ? "stop" : "tool_calls";
  apply_reasoning_language_metadata(
    result, request_.language, llm_reasoning_language_control::prompt_contract);
  completed_ = true;
  return false; // Stop at the protocol terminal, not at a possibly never-ending HTTP EOF.
}

bool decoder::event(const sse_event& value) {
  if (terminal())
    return false;
  if (value.data.empty())
    return true;
  if (value.data == "[DONE]")
    return fail(); // Not a substitute for response.completed.
  const auto data = parse(value.data);
  if (!data.is_object())
    return fail();
  const auto type = string_field(data, "type");
  if (type.empty() || (!value.event.empty() && value.event != type))
    return fail();
  if (data.contains("response") && data["response"].is_object()) {
    auto id = string_field(data["response"], "id");
    if (!id.empty()) {
      if (!safe_text(id, 1024) || (!response_id_.empty() && response_id_ != id))
        return fail();
      response_id_ = id;
      result.metadata["response_id"] = std::move(id);
    }
  }
  if (type == "error")
    return fail(data.contains("error") ? server_error(data["error"]) : server_error(data));
  if (type == "response.failed")
    return fail(data.contains("response") && data["response"].contains("error")
                  ? server_error(data["response"]["error"])
                  : llm_error_code::api_error);
  if (type == "response.incomplete") {
    result.finish_reason = "incomplete";
    return fail(llm_error_code::api_error);
  }
  if (type == "response.completed")
    return data.contains("response") ? complete(data["response"]) : fail();
  const bool added = type == "response.output_item.added";
  const bool done = type == "response.output_item.done";
  const bool text = type == "response.output_text.delta" || type == "response.output_text.done" ||
                    type == "response.refusal.delta" || type == "response.refusal.done";
  const bool summary = type == "response.reasoning_summary_text.delta" ||
                       type == "response.reasoning_summary_text.done";
  const bool arguments = type == "response.function_call_arguments.delta" ||
                         type == "response.function_call_arguments.done";
  if (!added && !done && !text && !summary && !arguments)
    return true;
  const auto value_item = (added || done) && data.contains("item") ? data["item"] : json();
  const auto id = added || done ? string_field(value_item, "id") : string_field(data, "item_id");
  std::optional<int> index;
  if (data.contains("output_index"))
    index = integer(data["output_index"], options_.max_output_items - 1);
  else if (ids_.contains(id))
    index = ids_.at(id);
  else if (added || done)
    index = static_cast<int>(items_.size());
  if (!index)
    return fail();
  if (done)
    return finalize_item(*index, value_item);
  const auto kind = added     ? string_field(value_item, "type")
                    : text    ? "message"
                    : summary ? "reasoning"
                              : "function_call";
  if (kind != "message" && kind != "reasoning" && kind != "function_call")
    return fail(llm_error_code::unsupported_capability);
  auto* state = item(*index, id, kind);
  if (!state || state->done)
    return fail();
  if (added) {
    if (kind == "function_call") {
      if (!state->call.id.empty())
        return fail();
      state->call = { string_field(value_item, "call_id"),
        string_field(value_item, "name"),
        string_field(value_item, "arguments") };
      if (!safe_text(state->call.id, 1024) || !function_name(state->call.name))
        return fail();
      state->arguments_seen = !state->call.arguments_json.empty();
      emit_({ .type = llm_stream_event_type::tool_call_delta,
        .tool_call_delta = llm_tool_call_delta {
          *index, state->call.id, state->call.name, state->call.arguments_json } });
    }
    return true;
  }
  const bool delta = type.ends_with(".delta");
  const char* field = delta                             ? "delta"
                      : arguments                       ? "arguments"
                      : type == "response.refusal.done" ? "refusal"
                                                        : "text";
  if (!data.contains(field) || !data[field].is_string())
    return fail();
  const auto fragment = data[field].get<std::string>();
  if (arguments) {
    if (state->call.id.empty())
      return fail();
    if (!delta && !fragment.starts_with(state->call.arguments_json))
      return fail();
    auto suffix = delta ? fragment : fragment.substr(state->call.arguments_json.size());
    state->call.arguments_json += suffix;
    state->arguments_seen = true;
    if (!suffix.empty())
      emit_({ .type = llm_stream_event_type::tool_call_delta,
        .tool_call_delta = llm_tool_call_delta { *index, {}, {}, std::move(suffix) } });
  }
  else {
    const char* key = summary ? "summary_index" : "content_index";
    auto part = data.contains(key) ? integer(data[key], options_.max_output_items - 1)
                                   : std::optional<int>(0);
    if (!part)
      return fail();
    auto& previous = summary ? state->summary[*part] : state->text[*part];
    if (!delta && !fragment.starts_with(previous))
      return fail();
    auto suffix = delta ? fragment : fragment.substr(previous.size());
    previous += suffix;
    if (!suffix.empty()) {
      if (summary) {
        result.reasoning_summary += suffix;
        emit_(
          { .type = llm_stream_event_type::reasoning_delta, .reasoning_delta = std::move(suffix) });
      }
      else {
        result.content += suffix;
        emit_({ .type = llm_stream_event_type::content_delta, .content_delta = std::move(suffix) });
      }
    }
  }
  return true;
}
} // namespace codex_responses
WUWE_NAMESPACE_END
