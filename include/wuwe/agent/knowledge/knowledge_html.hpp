#ifndef WUWE_AGENT_KNOWLEDGE_HTML_HPP
#define WUWE_AGENT_KNOWLEDGE_HTML_HPP

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

#include <wuwe/agent/knowledge/knowledge_text.hpp>

namespace wuwe::agent::knowledge {

enum class html_text_mode {
  plain,
  structured,
};

class html_text_extractor {
public:
  static std::string to_text(const std::string& html, html_text_mode mode = html_text_mode::plain) {
    return html_text_extractor(html, mode).extract();
  }

  static std::string title(const std::string& html) {
    const auto title_start = text_detail::find_case_insensitive(html, "<title", 0);
    if (title_start == std::string::npos) {
      return {};
    }
    const auto tag_end = html.find('>', title_start);
    if (tag_end == std::string::npos) {
      return {};
    }
    const auto title_end = text_detail::find_case_insensitive(html, "</title>", tag_end + 1);
    if (title_end == std::string::npos) {
      return {};
    }
    return text_detail::trim_copy(
      to_text(html.substr(tag_end + 1, title_end - tag_end - 1), html_text_mode::plain));
  }

private:
  html_text_extractor(const std::string& html, html_text_mode mode)
      : html_(html), mode_(mode) {
  }

  std::string extract() {
    output_.reserve(html_.size());
    for (std::size_t index = 0; index < html_.size();) {
      if (skip_element(index, "script", 9) || skip_element(index, "style", 8)) {
        continue;
      }
      if (html_[index] == '<') {
        consume_tag(index);
        continue;
      }
      if (html_[index] == '&' && consume_entity(index)) {
        continue;
      }
      consume_text_byte(index);
    }
    return text_detail::trim_copy(output_);
  }

  bool skip_element(std::size_t& index, std::string_view tag_name, std::size_t close_size) {
    const auto open = "<" + std::string(tag_name);
    if (!starts_with_case_insensitive(html_, index, open)) {
      return false;
    }
    const auto close = "</" + std::string(tag_name) + ">";
    const auto end = text_detail::find_case_insensitive(html_, close, index);
    index = end == std::string::npos ? html_.size() : end + close_size;
    append_space();
    return true;
  }

  void consume_tag(std::size_t& index) {
    const auto end = html_.find('>', index + 1);
    if (end == std::string::npos) {
      index = html_.size();
      return;
    }

    const auto tag = std::string_view(html_).substr(index, end - index + 1);
    const auto name = tag_name(tag);
    const bool closing = tag.size() > 1 && tag[1] == '/';

    if (mode_ == html_text_mode::structured) {
      append_structured_boundary(name, closing);
    }
    else {
      append_plain_boundary(name);
    }
    index = end + 1;
  }

  bool consume_entity(std::size_t& index) {
    const auto end = html_.find(';', index + 1);
    if (end == std::string::npos || end - index > 12) {
      return false;
    }
    output_ += decode_entity(std::string_view(html_).substr(index + 1, end - index - 1));
    index = end + 1;
    return true;
  }

  void consume_text_byte(std::size_t& index) {
    const auto ch = html_[index];
    if (ch == '\r') {
      ++index;
      return;
    }
    if (ch == '\n') {
      append_newline();
    }
    else if (std::isspace(static_cast<unsigned char>(ch))) {
      append_space();
    }
    else {
      output_.push_back(ch);
    }
    ++index;
  }

  void append_plain_boundary(std::string_view name) {
    if (name == "br" || name == "p" || name == "div") {
      append_newline();
    }
    else {
      append_space();
    }
  }

  void append_structured_boundary(std::string_view name, bool closing) {
    if (!closing && is_heading(name)) {
      append_blank_line();
      output_ += "# ";
    }
    else if (closing && is_heading(name)) {
      append_blank_line();
    }
    else if (name == "br" || name == "hr") {
      append_newline();
    }
    else if (name == "p" || name == "div" || name == "section" || name == "article" ||
             name == "main" || name == "table" || name == "tr" || name == "pre" ||
             name == "blockquote") {
      append_blank_line();
    }
    else if (name == "li") {
      append_newline();
      if (!closing) {
        output_ += "- ";
      }
    }
    else {
      append_space();
    }
  }

  void append_space() {
    if (!output_.empty() && output_.back() != ' ' && output_.back() != '\n') {
      output_.push_back(' ');
    }
  }

  void append_newline() {
    while (!output_.empty() && output_.back() == ' ') {
      output_.pop_back();
    }
    if (!output_.empty() && output_.back() != '\n') {
      output_.push_back('\n');
    }
  }

  void append_blank_line() {
    append_newline();
    if (!output_.empty() && output_.size() >= 2 && output_[output_.size() - 2] != '\n') {
      output_.push_back('\n');
    }
  }

  static bool starts_with_case_insensitive(
    const std::string& text,
    std::size_t offset,
    std::string_view prefix) {
    if (offset + prefix.size() > text.size()) {
      return false;
    }
    for (std::size_t index = 0; index < prefix.size(); ++index) {
      const auto lhs = static_cast<unsigned char>(text[offset + index]);
      const auto rhs = static_cast<unsigned char>(prefix[index]);
      if (std::tolower(lhs) != std::tolower(rhs)) {
        return false;
      }
    }
    return true;
  }

  static std::string tag_name(std::string_view tag) {
    std::size_t index = 1;
    if (index < tag.size() && tag[index] == '/') {
      ++index;
    }
    while (index < tag.size() && std::isspace(static_cast<unsigned char>(tag[index]))) {
      ++index;
    }
    const auto start = index;
    while (index < tag.size() && std::isalnum(static_cast<unsigned char>(tag[index]))) {
      ++index;
    }
    auto name = std::string(tag.substr(start, index - start));
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) {
      return static_cast<char>(std::tolower(ch));
    });
    return name;
  }

  static bool is_heading(std::string_view name) {
    return name.size() == 2 && name.front() == 'h' && name.back() >= '1' && name.back() <= '6';
  }

  static std::string decode_entity(std::string_view entity) {
    if (entity == "amp") {
      return "&";
    }
    if (entity == "lt") {
      return "<";
    }
    if (entity == "gt") {
      return ">";
    }
    if (entity == "quot") {
      return "\"";
    }
    if (entity == "apos") {
      return "'";
    }
    if (entity == "nbsp") {
      return " ";
    }
    return "&" + std::string(entity) + ";";
  }

  const std::string& html_;
  html_text_mode mode_;
  std::string output_;
};

class utf8_sanitizer {
public:
  static std::string sanitize(const std::string& text) {
    std::string output;
    output.reserve(text.size());

    for (std::size_t index = 0; index < text.size();) {
      const auto byte = static_cast<unsigned char>(text[index]);
      if (byte < 0x80) {
        output.push_back(text[index++]);
        continue;
      }

      const auto length = utf8_sequence_length(byte);
      if (length == 0) {
        output.push_back('?');
        ++index;
        continue;
      }
      if (index + length > text.size()) {
        output.push_back('?');
        break;
      }
      if (!valid_continuation(text, index, length)) {
        output.push_back('?');
        ++index;
        continue;
      }
      output.append(text, index, length);
      index += length;
    }
    return output;
  }

private:
  static std::size_t utf8_sequence_length(unsigned char byte) {
    if ((byte & 0xE0) == 0xC0) {
      return 2;
    }
    if ((byte & 0xF0) == 0xE0) {
      return 3;
    }
    if ((byte & 0xF8) == 0xF0) {
      return 4;
    }
    return 0;
  }

  static bool valid_continuation(const std::string& text, std::size_t index, std::size_t length) {
    for (std::size_t offset = 1; offset < length; ++offset) {
      const auto continuation = static_cast<unsigned char>(text[index + offset]);
      if ((continuation & 0xC0) != 0x80) {
        return false;
      }
    }
    return true;
  }
};

} // namespace wuwe::agent::knowledge

#endif // WUWE_AGENT_KNOWLEDGE_HTML_HPP
