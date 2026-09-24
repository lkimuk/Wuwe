#include <wuwe/agent/llm/codex_llm_client.h>

#include "../auth/codex_oauth_protocol.h"
#include "codex_responses.h"
#include "llm_stream_timeouts.hpp"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <wuwe/net/net_errc.h>
#include <wuwe/net/transport_error.h>

WUWE_NAMESPACE_BEGIN
namespace {
using agent::llm_error_code;
bool callback_abort(const std::error_code& error) {
  return !error || error == transport_error::aborted_by_callback ||
         error == transport_error::write_error || error == std::errc::operation_canceled;
}
std::error_code http_error(int status) {
  if (status == 401 || status == 403)
    return agent::make_error_code(llm_error_code::authentication_failed);
  if (status == 429)
    return agent::make_error_code(llm_error_code::rate_limited);
  if (status == 404)
    return agent::make_error_code(llm_error_code::model_unavailable);
  if (status == 400 || status == 422)
    return agent::make_error_code(llm_error_code::invalid_request);
  return agent::make_error_code(llm_error_code::http_error);
}
void validate(const codex_llm_options& options, const oauth_account_key& account) {
  using codex_detail::safe_text;
  const auto& t = options.stream_timeouts;
  if (account.provider != codex_detail::provider || !safe_text(account.account_id, 256) ||
      !safe_text(options.model, 256, false) || !safe_text(options.client_version, 128) ||
      !safe_text(options.originator, 128) || options.timeout_ms <= 0 ||
      options.timeout_ms > 3'600'000 || t.total_ms < 0 || t.total_ms > 3'600'000 ||
      t.connect_ms < 0 || t.first_event_ms < 0 || t.idle_ms < 0 || options.max_request_bytes == 0 ||
      options.max_request_bytes > 64 * 1024 * 1024 || options.max_response_bytes == 0 ||
      options.max_response_bytes > 64 * 1024 * 1024 || options.max_event_bytes == 0 ||
      options.max_event_bytes > options.max_response_bytes || options.max_output_items == 0 ||
      options.max_output_items > 10'000)
    throw std::invalid_argument("Invalid Codex generation configuration");
  if (options.reasoning_effort) {
    const auto& e = *options.reasoning_effort;
    if (e != "none" && e != "minimal" && e != "low" && e != "medium" && e != "high" && e != "xhigh")
      throw std::invalid_argument("Invalid Codex reasoning effort");
  }
}
} // namespace

class codex_llm_client::impl {
public:
  oauth_account_manager& accounts;
  oauth_account_key account;
  codex_llm_options options;
  std::shared_ptr<http_client> http;
  impl(oauth_account_manager& manager, oauth_account_key key, codex_llm_options config,
    std::shared_ptr<http_client> transport)
      : accounts(manager), account(std::move(key)), options(std::move(config)),
        http(codex_detail::transport(std::move(transport))) {
    validate(options, account);
  }
};
codex_llm_client::codex_llm_client(oauth_account_manager& accounts, oauth_account_key account,
  codex_llm_options options, std::shared_ptr<http_client> http)
    : impl_(
        std::make_unique<impl>(accounts, std::move(account), std::move(options), std::move(http))) {
}
codex_llm_client::~codex_llm_client() = default;
llm_provider_capabilities codex_llm_client::capabilities() const noexcept {
  return { .streaming = true,
    .tools = true,
    .tool_choice = true,
    .reasoning_summary = true,
    .streaming_reasoning_summary = true,
    .reasoning_language_control = llm_reasoning_language_control::prompt_contract,
    .json_schema_output = true,
    .provider_state = true };
}
llm_response codex_llm_client::complete(const llm_request& request) {
  return complete(request, {});
}
llm_response codex_llm_client::complete(const llm_request& request, std::stop_token stop) {
  return complete_stream(request, {}, stop);
}
llm_response codex_llm_client::complete_stream(
  const llm_request& request, const llm_stream_callbacks& callbacks, std::stop_token stop) {
  const auto emit = [&](llm_stream_event event) {
    if (callbacks.on_event)
      callbacks.on_event(event);
    if (event.type == llm_stream_event_type::reasoning_delta && callbacks.on_reasoning_delta)
      callbacks.on_reasoning_delta(event.reasoning_delta);
    if (event.type == llm_stream_event_type::reasoning_done && callbacks.on_reasoning_done)
      callbacks.on_reasoning_done(event.reasoning_summary);
  };
  const auto fail = [&](std::error_code error, llm_response result = {}) {
    result.error_code = error;
    result.tool_calls.clear();
    result.provider_state.reset();
    emit({ .type = llm_stream_event_type::error,
      .response = result,
      .error_code = error,
      .message = error.category() == oauth_error_category() ? "Codex account operation failed"
                                                            : error.message() });
    return result;
  };
  if (stop.stop_requested())
    return fail(agent::make_error_code(llm_error_code::cancelled));
  auto snapshot = impl_->accounts.account_snapshot(impl_->account);
  if (snapshot.error)
    return fail(snapshot.error);
  if (!codex_detail::safe_text(snapshot.account->subject_id, 1024) ||
      !codex_detail::safe_text(snapshot.account->upstream_account_id, 1024))
    return fail(make_error_code(oauth_error::invalid_argument));
  codex_responses::payload_result payload;
  std::string serialized;
  try {
    payload = codex_responses::build_payload(request, impl_->options, *snapshot.account);
    if (!payload.error)
      serialized = payload.body.dump();
  }
  catch (...) {
    payload.error = llm_error_code::invalid_request;
  }
  if (payload.error) {
    llm_response result;
    if (!payload.unsupported.empty())
      result.metadata["unsupported_capability"] = payload.unsupported;
    return fail(payload.error, std::move(result));
  }
  if (serialized.size() > impl_->options.max_request_bytes)
    return fail(agent::make_error_code(llm_error_code::invalid_request));
  const auto access = impl_->accounts.access_token(impl_->account, stop);
  if (access.error)
    return fail(access.error == oauth_error::cancelled
                  ? agent::make_error_code(llm_error_code::cancelled)
                  : access.error);
  if (snapshot.revision != access.revision)
    return fail(make_error_code(oauth_error::account_changed));

  const auto& options = impl_->options;
  const auto total_ms =
    options.stream_timeouts.total_ms > 0 ? options.stream_timeouts.total_ms : options.timeout_ms;
  agent::llm_detail::stream_deadline_monitor deadline(options.stream_timeouts, total_ms);
  std::stop_source io_stop;
  std::stop_callback caller_stop(stop, [&] { io_stop.request_stop(); });
  std::stop_callback deadline_stop(deadline.token(), [&] { io_stop.request_stop(); });
  std::string timeout_phase;
  std::error_code local_error;
  const auto current = [&] {
    const auto account = impl_->accounts.account_snapshot(impl_->account);
    return !account.error && account.revision == access.revision;
  };
  const auto check = [&] {
    if (stop.stop_requested())
      local_error = llm_error_code::cancelled;
    else if (!current())
      local_error = oauth_error::account_changed;
    else if (const auto phase = deadline.phase(); !phase.empty()) {
      local_error = llm_error_code::timeout;
      timeout_phase = phase;
    }
    return !local_error;
  };
  // Capture callback exceptions inside the C transport boundary; rethrow only
  // after send_stream returns, never unwind through libcurl's C callback stack.
  std::exception_ptr callback_exception;
  const auto safe_emit = [&](llm_stream_event event) {
    if (callback_exception || !check())
      return;
    try {
      emit(std::move(event));
    }
    catch (...) {
      callback_exception = std::current_exception();
    }
  };
  codex_responses::decoder decoder(options,
    request,
    access.account,
    request.model.empty() ? options.model : request.model,
    safe_emit);
  sse_event_parser parser(options.max_event_bytes);
  const auto event = [&](const sse_event& value) {
    if (!check() || callback_exception)
      return false;
    if (!value.data.empty())
      deadline.mark_event();
    const bool keep = decoder.event(value);
    return keep && !callback_exception && check();
  };
  std::size_t received = 0;
  bool dispatched = false;
  bool terminal_abort = false;
  const auto chunk = [&](std::string_view data) {
    dispatched = true;
    if (!check() || callback_exception)
      return false;
    if (data.size() > options.max_response_bytes - received) {
      local_error = llm_error_code::response_limit_exceeded;
      return false;
    }
    received += data.size();
    try {
      const auto keep = parser.feed(data, event);
      if (parser.limit_exceeded())
        local_error = llm_error_code::response_limit_exceeded;
      terminal_abort = decoder.terminal() && !local_error && !callback_exception;
      return keep && !callback_exception && !local_error;
    }
    catch (...) {
      local_error = llm_error_code::invalid_response;
      return false;
    }
  };
  http_request http_request { .method = "POST",
    .url = "https://chatgpt.com/backend-api/codex/responses",
    .headers = codex_detail::account_headers(
      access, options.originator, options.client_version, "text/event-stream"),
    .body = std::move(serialized),
    .timeout = total_ms,
    .timeouts = { .total_ms = total_ms,
      .connect_ms = options.stream_timeouts.connect_ms,
      .read_ms = agent::llm_detail::first_positive(
        { options.stream_timeouts.idle_ms, options.stream_timeouts.first_event_ms }) },
    .follow_redirects = false };
  http_request.headers.emplace_back("Content-Type", "application/json");
  http_response http_response;
  try {
    http_response = impl_->http->send_stream(http_request, chunk, io_stop.get_token());
  }
  catch (...) {
    if (!callback_exception)
      local_error = llm_error_code::transport_error;
  }
  if (callback_exception)
    std::rethrow_exception(callback_exception);
  // Buffered injected transports may not call the chunk callback on HTTP errors.
  // Do not parse their error bodies or expose them in diagnostic messages.
  if (!dispatched && !http_response.error_code && !http_response.transport_error &&
      http_response.status_code >= 200 && http_response.status_code < 300 &&
      !http_response.body.empty())
    chunk(http_response.body);
  if (callback_exception)
    std::rethrow_exception(callback_exception);
  if (!decoder.terminal() && !local_error && dispatched) {
    try {
      parser.finish(event);
    }
    catch (...) {
      local_error = llm_error_code::invalid_response;
    }
  }
  if (callback_exception)
    std::rethrow_exception(callback_exception);
  if (!decoder.completed() && !local_error)
    check();
  auto result = std::move(decoder.result);
  result.metadata["http_status"] = std::to_string(http_response.status_code);
  if (!timeout_phase.empty())
    result.metadata["timeout_phase"] = timeout_phase;
  if (stop.stop_requested())
    return fail(agent::make_error_code(llm_error_code::cancelled), std::move(result));
  if (!current())
    return fail(make_error_code(oauth_error::account_changed), std::move(result));
  if (local_error)
    return fail(local_error, std::move(result));
  // A deliberate terminal callback stop may surface as a write/callback error.
  const auto network_error =
    http_response.transport_error ? http_response.transport_error : http_response.error_code;
  const bool intentional_abort = terminal_abort && callback_abort(network_error);
  if (network_error && network_error.category() != http_status_category() && !intentional_abort) {
    return fail(agent::make_error_code(
                  network_error == net_errc::timeout || network_error == std::errc::timed_out
                    ? llm_error_code::timeout
                    : llm_error_code::transport_error),
      std::move(result));
  }
  if (http_response.status_code < 200 || http_response.status_code >= 300)
    return fail(http_error(http_response.status_code), std::move(result));
  if (result.error_code)
    return fail(result.error_code, std::move(result));
  if (!decoder.completed())
    return fail(agent::make_error_code(llm_error_code::invalid_response), std::move(result));
  // Success notifications are a commit point; callbacks may initiate later
  // application actions, but cannot make an incomplete stream executable.
  if (!result.reasoning_summary.empty())
    emit({ .type = llm_stream_event_type::reasoning_done,
      .reasoning_summary = result.reasoning_summary,
      .reasoning_metadata = result.reasoning_metadata });
  for (const auto& call : result.tool_calls)
    emit({ .type = llm_stream_event_type::tool_call_done, .tool_call = call });
  emit({ .type = llm_stream_event_type::done, .response = result });
  return result;
}
WUWE_NAMESPACE_END
