#ifndef WUWE_AGENT_KNOWLEDGE_FILE_KNOWLEDGE_LOADER_HPP
#define WUWE_AGENT_KNOWLEDGE_FILE_KNOWLEDGE_LOADER_HPP

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <wuwe/agent/knowledge/knowledge_record.hpp>
#include <wuwe/agent/knowledge/knowledge_hash.hpp>

namespace wuwe::agent::knowledge {

struct file_knowledge_loader_options {
  std::string id;
  std::string title;
  std::string source_uri;
  std::map<std::string, std::string> metadata;
  bool extract_html_text { true };
  bool extract_rtf_text { true };
};

class file_knowledge_loader {
public:
  knowledge_document load(
    const std::filesystem::path& path,
    file_knowledge_loader_options options = {}) const {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
      throw std::runtime_error("failed to open knowledge file: " + path.string());
    }

    std::ostringstream content;
    content << input.rdbuf();

    knowledge_document document;
    document.id = options.id.empty() ? default_id(path) : std::move(options.id);
    document.title = options.title.empty() ? path.stem().string() : std::move(options.title);
    const auto extension = lowercase_extension(path);
    const auto raw_content = content.str();
    if (extension_is_html(extension) && options.extract_html_text) {
      document.content = html_to_text(raw_content);
    }
    else if (extension_is_rtf(extension) && options.extract_rtf_text) {
      document.content = rtf_to_text(raw_content);
    }
    else {
      document.content = raw_content;
    }
    document.source_uri =
      options.source_uri.empty() ? path.generic_string() : std::move(options.source_uri);
    document.metadata = std::move(options.metadata);

    if (!extension.empty()) {
      document.metadata.try_emplace("extension", extension);
      if (extension_is_html(extension) && options.extract_html_text) {
        document.metadata.try_emplace("content_type", "text/html");
        document.metadata.try_emplace("extracted_as", "text");
      }
      if (extension_is_rtf(extension) && options.extract_rtf_text) {
        document.metadata.try_emplace("content_type", "application/rtf");
        document.metadata.try_emplace("extracted_as", "text");
      }
    }
    document.metadata["content_hash"] = stable_hash(document.content);
    return document;
  }

  static std::string default_id(const std::filesystem::path& path) {
    auto id = path.filename().string();
    if (id.empty()) {
      id = path.string();
    }
    for (auto& ch : id) {
      if (ch == '\\' || ch == '/' || ch == ' ' || ch == ':') {
        ch = '-';
      }
    }
    return id;
  }

private:
  static std::string lowercase_extension(const std::filesystem::path& path) {
    auto extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
      return static_cast<char>(std::tolower(ch));
    });
    return extension;
  }

  static bool extension_is_html(const std::string& extension) {
    return extension == ".html" || extension == ".htm";
  }

  static bool extension_is_rtf(const std::string& extension) {
    return extension == ".rtf";
  }

  static bool starts_with_tag(
    const std::string& text,
    std::size_t offset,
    std::string_view tag) {
    if (offset + tag.size() > text.size()) {
      return false;
    }
    for (std::size_t index = 0; index < tag.size(); ++index) {
      const auto lhs = static_cast<unsigned char>(text[offset + index]);
      const auto rhs = static_cast<unsigned char>(tag[index]);
      if (std::tolower(lhs) != std::tolower(rhs)) {
        return false;
      }
    }
    return true;
  }

  static std::string decode_html_entity(std::string_view entity) {
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

  static void append_space(std::string& output) {
    if (!output.empty() && output.back() != ' ' && output.back() != '\n') {
      output.push_back(' ');
    }
  }

  static std::string html_to_text(const std::string& html) {
    std::string output;
    output.reserve(html.size());

    for (std::size_t index = 0; index < html.size();) {
      if (starts_with_tag(html, index, "<script")) {
        const auto end = find_case_insensitive(html, "</script>", index);
        index = end == std::string::npos ? html.size() : end + 9;
        append_space(output);
        continue;
      }
      if (starts_with_tag(html, index, "<style")) {
        const auto end = find_case_insensitive(html, "</style>", index);
        index = end == std::string::npos ? html.size() : end + 8;
        append_space(output);
        continue;
      }

      if (html[index] == '<') {
        const auto end = html.find('>', index + 1);
        index = end == std::string::npos ? html.size() : end + 1;
        append_space(output);
        continue;
      }

      if (html[index] == '&') {
        const auto end = html.find(';', index + 1);
        if (end != std::string::npos && end - index <= 12) {
          output += decode_html_entity(std::string_view(html).substr(index + 1, end - index - 1));
          index = end + 1;
          continue;
        }
      }

      if (std::isspace(static_cast<unsigned char>(html[index]))) {
        append_space(output);
      }
      else {
        output.push_back(html[index]);
      }
      ++index;
    }
    return output;
  }

  static int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') {
      return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
      return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
      return ch - 'A' + 10;
    }
    return -1;
  }

  static bool is_ignorable_rtf_destination(std::string_view word) {
    return word == "fonttbl" || word == "colortbl" || word == "stylesheet" ||
           word == "info" || word == "pict" || word == "object" ||
           word == "datastore" || word == "themedata";
  }

  static std::string rtf_to_text(const std::string& rtf) {
    struct group_state {
      bool ignorable {};
    };

    std::vector<group_state> stack { {} };
    std::string output;
    output.reserve(rtf.size());

    auto append_text_space = [&] {
      if (!output.empty() && output.back() != ' ' && output.back() != '\n') {
        output.push_back(' ');
      }
    };

    for (std::size_t index = 0; index < rtf.size();) {
      const auto ch = rtf[index];
      if (ch == '{') {
        stack.push_back(stack.back());
        ++index;
        continue;
      }
      if (ch == '}') {
        if (stack.size() > 1) {
          stack.pop_back();
        }
        ++index;
        continue;
      }
      if (ch != '\\') {
        if (!stack.back().ignorable) {
          if (std::isspace(static_cast<unsigned char>(ch))) {
            append_text_space();
          }
          else {
            output.push_back(ch);
          }
        }
        ++index;
        continue;
      }

      ++index;
      if (index >= rtf.size()) {
        break;
      }

      const auto escaped = rtf[index];
      if (escaped == '\\' || escaped == '{' || escaped == '}') {
        if (!stack.back().ignorable) {
          output.push_back(escaped);
        }
        ++index;
        continue;
      }
      if (escaped == '~') {
        if (!stack.back().ignorable) {
          append_text_space();
        }
        ++index;
        continue;
      }
      if (escaped == '*') {
        stack.back().ignorable = true;
        ++index;
        continue;
      }
      if (escaped == '\'') {
        if (index + 2 < rtf.size()) {
          const auto high = hex_value(rtf[index + 1]);
          const auto low = hex_value(rtf[index + 2]);
          if (high >= 0 && low >= 0 && !stack.back().ignorable) {
            output.push_back(static_cast<char>((high << 4) | low));
          }
          index += 3;
          continue;
        }
      }

      if (!std::isalpha(static_cast<unsigned char>(escaped))) {
        if (!stack.back().ignorable && !std::isspace(static_cast<unsigned char>(escaped))) {
          output.push_back(escaped);
        }
        ++index;
        continue;
      }

      const auto word_start = index;
      while (index < rtf.size() && std::isalpha(static_cast<unsigned char>(rtf[index]))) {
        ++index;
      }
      const auto word = std::string_view(rtf).substr(word_start, index - word_start);

      if (index < rtf.size() && (rtf[index] == '-' || std::isdigit(static_cast<unsigned char>(rtf[index])))) {
        ++index;
        while (index < rtf.size() && std::isdigit(static_cast<unsigned char>(rtf[index]))) {
          ++index;
        }
      }
      if (index < rtf.size() && rtf[index] == ' ') {
        ++index;
      }

      if (is_ignorable_rtf_destination(word)) {
        stack.back().ignorable = true;
        continue;
      }
      if (stack.back().ignorable) {
        continue;
      }
      if (word == "par" || word == "line") {
        if (!output.empty() && output.back() != '\n') {
          output.push_back('\n');
        }
      }
      else if (word == "tab") {
        append_text_space();
      }
    }
    return output;
  }

  static std::size_t find_case_insensitive(
    const std::string& text,
    std::string_view needle,
    std::size_t offset) {
    for (std::size_t index = offset; index + needle.size() <= text.size(); ++index) {
      if (starts_with_tag(text, index, needle)) {
        return index;
      }
    }
    return std::string::npos;
  }
};

} // namespace wuwe::agent::knowledge

#endif // WUWE_AGENT_KNOWLEDGE_FILE_KNOWLEDGE_LOADER_HPP
