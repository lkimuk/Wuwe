#ifndef WUWE_NET_SSE_EVENT_PARSER_H
#define WUWE_NET_SSE_EVENT_PARSER_H

#include <functional>
#include <string>
#include <string_view>

#include <wuwe/common/wuwe_fwd.h>

WUWE_NAMESPACE_BEGIN

struct sse_event {
  std::string event;
  std::string data;
  std::string id;
};

class sse_event_parser {
public:
  using callback_type = std::function<bool(const sse_event&)>;

  bool feed(std::string_view chunk, const callback_type& callback) {
    buffer_.append(chunk);

    std::size_t line_end = buffer_.find('\n');
    while (line_end != std::string::npos) {
      auto line = buffer_.substr(0, line_end);
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      buffer_.erase(0, line_end + 1);

      if (!process_line(line, callback)) {
        return false;
      }
      line_end = buffer_.find('\n');
    }

    return true;
  }

  bool finish(const callback_type& callback) {
    if (!buffer_.empty()) {
      auto line = std::move(buffer_);
      buffer_.clear();
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      if (!process_line(line, callback)) {
        return false;
      }
    }

    if (has_pending_event()) {
      return dispatch(callback);
    }
    return true;
  }

  void reset() {
    buffer_.clear();
    pending_ = {};
    has_data_ = false;
    has_event_field_ = false;
  }

private:
  bool process_line(std::string_view line, const callback_type& callback) {
    if (line.empty()) {
      if (has_pending_event()) {
        return dispatch(callback);
      }
      return true;
    }

    if (line.front() == ':') {
      return true;
    }

    const auto separator = line.find(':');
    const auto field = line.substr(0, separator);
    auto value = separator == std::string_view::npos
                   ? std::string_view {}
                   : line.substr(separator + 1);
    if (!value.empty() && value.front() == ' ') {
      value.remove_prefix(1);
    }

    if (field == "event") {
      pending_.event = std::string(value);
      has_event_field_ = true;
    }
    else if (field == "data") {
      if (has_data_) {
        pending_.data.push_back('\n');
      }
      pending_.data.append(value);
      has_data_ = true;
    }
    else if (field == "id") {
      pending_.id = std::string(value);
    }

    return true;
  }

  bool dispatch(const callback_type& callback) {
    auto event = std::move(pending_);
    pending_ = {};
    has_data_ = false;
    has_event_field_ = false;
    return !callback || callback(event);
  }

  bool has_pending_event() const {
    return has_data_ || has_event_field_ || !pending_.id.empty();
  }

  std::string buffer_;
  sse_event pending_;
  bool has_data_ { false };
  bool has_event_field_ { false };
};

WUWE_NAMESPACE_END

#endif // WUWE_NET_SSE_EVENT_PARSER_H
