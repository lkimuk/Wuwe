#ifndef WUWE_AGENT_KNOWLEDGE_SPLITTER_HPP
#define WUWE_AGENT_KNOWLEDGE_SPLITTER_HPP

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <wuwe/agent/knowledge/knowledge_record.hpp>
#include <wuwe/agent/knowledge/knowledge_text.hpp>

namespace wuwe::agent::knowledge {

struct chunking_policy {
  std::size_t max_chars { 1200 };
  std::size_t overlap_chars { 160 };
  std::size_t max_tokens {};
  std::size_t overlap_tokens {};
  bool respect_markdown_headings { true };
  bool prefer_paragraph_boundaries { true };
  bool protect_markdown_code_fences { true };
  bool respect_code_symbols { true };
  bool include_document_summary_chunk {};
  std::size_t document_summary_chars { 6000 };
};

class knowledge_splitter {
public:
  explicit knowledge_splitter(chunking_policy policy = {}) : policy_(policy) {
    if (policy_.max_chars == 0) {
      throw std::invalid_argument("knowledge_splitter max_chars must be greater than zero");
    }
    if (policy_.overlap_chars >= policy_.max_chars) {
      policy_.overlap_chars = policy_.max_chars / 4;
    }
    if (policy_.max_tokens != 0 && policy_.overlap_tokens >= policy_.max_tokens) {
      policy_.overlap_tokens = policy_.max_tokens / 4;
    }
  }

  std::vector<knowledge_chunk> split(const knowledge_document& document) const {
    std::vector<knowledge_chunk> chunks;
    if (document.content.empty()) {
      return chunks;
    }

    if (policy_.respect_markdown_headings && should_respect_markdown_headings(document)) {
      chunks = split_markdown_sections(document);
      if (!chunks.empty()) {
        prepend_document_summary_chunk(document, chunks);
        return chunks;
      }
    }

    if (policy_.respect_code_symbols && is_code_document(document)) {
      chunks = split_code_symbols(document);
      if (!chunks.empty()) {
        prepend_document_summary_chunk(document, chunks);
        return chunks;
      }
    }

    append_range_chunks(document, 0, document.content.size(), {}, chunks);
    prepend_document_summary_chunk(document, chunks);
    return chunks;
  }

  const chunking_policy& policy() const noexcept {
    return policy_;
  }

private:
  struct markdown_section {
    std::size_t start {};
    std::size_t end {};
    std::string title;
  };

  struct token_span {
    std::size_t start {};
    std::size_t end {};
  };

  struct document_summary {
    std::string content;
    std::vector<std::string> toc_entries;
  };

  static bool is_markdown_heading(std::string_view line, std::string& title) {
    std::size_t level = 0;
    while (level < line.size() && line[level] == '#') {
      ++level;
    }
    if (level == 0 || level > 6 || level >= line.size()) {
      return false;
    }
    if (line[level] != ' ' && line[level] != '\t') {
      return false;
    }

    title = std::string(line.substr(level));
    while (!title.empty() && (title.front() == ' ' || title.front() == '\t')) {
      title.erase(title.begin());
    }
    while (!title.empty() && (title.back() == '\r' || title.back() == ' ' ||
                               title.back() == '\t')) {
      title.pop_back();
    }
    return !title.empty();
  }

  static bool should_respect_markdown_headings(const knowledge_document& document) {
    const auto parser = document.metadata.find("parser");
    if (parser != document.metadata.end() && parser->second == "tika") {
      return false;
    }

    const auto extension = document.metadata.find("extension");
    if (extension == document.metadata.end()) {
      return true;
    }
    return extension->second == ".md" || extension->second == ".markdown";
  }

  static bool is_markdown_code_fence(std::string_view line) {
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
      line.remove_prefix(1);
    }
    return line.starts_with("```") || line.starts_with("~~~");
  }

  static bool is_code_document(const knowledge_document& document) {
    const auto extension = document.metadata.find("extension");
    if (extension == document.metadata.end()) {
      return false;
    }
    return extension->second == ".c" || extension->second == ".cc" ||
           extension->second == ".cpp" || extension->second == ".cxx" ||
           extension->second == ".h" || extension->second == ".hpp" ||
           extension->second == ".hh" || extension->second == ".hxx" ||
           extension->second == ".js" || extension->second == ".ts" ||
           extension->second == ".jsx" || extension->second == ".tsx" ||
           extension->second == ".py" || extension->second == ".java" ||
           extension->second == ".cs" || extension->second == ".go" ||
           extension->second == ".rs";
  }

  static bool line_starts_with_keyword(std::string_view line, std::string_view keyword) {
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front()))) {
      line.remove_prefix(1);
    }
    if (!line.starts_with(keyword)) {
      return false;
    }
    return line.size() == keyword.size() ||
           !std::isalnum(static_cast<unsigned char>(line[keyword.size()]));
  }

  static bool looks_like_code_symbol_start(std::string_view line) {
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front()))) {
      line.remove_prefix(1);
    }
    if (line.empty() || line.starts_with("//") || line.starts_with("/*") ||
        line.starts_with("*") || line.starts_with("#include") || line.starts_with("import ")) {
      return false;
    }
    if (line_starts_with_keyword(line, "class") ||
        line_starts_with_keyword(line, "struct") ||
        line_starts_with_keyword(line, "enum") ||
        line_starts_with_keyword(line, "interface") ||
        line_starts_with_keyword(line, "namespace") ||
        line_starts_with_keyword(line, "def") ||
        line_starts_with_keyword(line, "fn") ||
        line_starts_with_keyword(line, "func") ||
        line_starts_with_keyword(line, "function")) {
      return true;
    }

    const auto paren = line.find('(');
    if (paren == std::string_view::npos) {
      return false;
    }
    const auto close = line.find(')', paren);
    if (close == std::string_view::npos) {
      return false;
    }
    const auto brace = line.find('{', close);
    const auto colon = line.find(':', close);
    return brace != std::string_view::npos || colon != std::string_view::npos;
  }

  static std::size_t line_for_offset(const std::string& content, std::size_t offset) {
    std::size_t line = 1;
    const auto end = (std::min)(offset, content.size());
    for (std::size_t index = 0; index < end; ++index) {
      if (content[index] == '\n') {
        ++line;
      }
    }
    return line;
  }

  static std::size_t page_for_offset(const std::string& content, std::size_t offset) {
    std::size_t page = 1;
    const auto end = (std::min)(offset, content.size());
    for (std::size_t index = 0; index < end; ++index) {
      if (content[index] == '\f') {
        ++page;
      }
    }
    return page;
  }

  static bool has_page_breaks(const knowledge_document& document) {
    return document.content.find('\f') != std::string::npos;
  }

  static bool is_tika_document(const knowledge_document& document) {
    const auto parser = document.metadata.find("parser");
    return parser != document.metadata.end() && parser->second == "tika";
  }

  static bool looks_like_plain_text_section(std::string_view line) {
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front()))) {
      line.remove_prefix(1);
    }
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) {
      line.remove_suffix(1);
    }
    if (line.empty() || line.size() > 120) {
      return false;
    }
    if (line.starts_with("Chapter ") || line.starts_with("CHAPTER ") ||
        line.starts_with("Part ") || line.starts_with("PART ")) {
      return true;
    }
    if (std::isdigit(static_cast<unsigned char>(line.front()))) {
      const auto dot = line.find('.');
      if (dot != std::string_view::npos && dot < 8 && dot + 2 < line.size()) {
        if (line.find("http") != std::string_view::npos ||
            std::count(line.begin(), line.end(), '.') > 1) {
          return false;
        }
        return true;
      }
    }
    return false;
  }

  static bool looks_like_document_summary_line(std::string_view line) {
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front()))) {
      line.remove_prefix(1);
    }
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) {
      line.remove_suffix(1);
    }
    if (line.empty() || line.size() > 160) {
      return false;
    }
    if (line.find("http") != std::string_view::npos) {
      return false;
    }
    if (line.starts_with("Chapter ") || line.starts_with("CHAPTER ") ||
        line.starts_with("Part ") || line.starts_with("PART ")) {
      return true;
    }
    if (line.find("Pattern") != std::string_view::npos ||
        line.find("pattern") != std::string_view::npos) {
      return true;
    }
    if (std::isdigit(static_cast<unsigned char>(line.front()))) {
      const auto dot = line.find('.');
      if (dot != std::string_view::npos && dot < 8 && dot + 2 < line.size() &&
          std::count(line.begin(), line.end(), '.') <= 1) {
        return true;
      }
    }
    return false;
  }

  static bool looks_like_toc_pattern_line(std::string_view line) {
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front()))) {
      line.remove_prefix(1);
    }
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) {
      line.remove_suffix(1);
    }
    if (line.empty() || line.size() > 140) {
      return false;
    }
    if (line.find("Pattern Overview") != std::string_view::npos ||
        line.find("Pattern (") != std::string_view::npos ||
        line.find("Patterns") != std::string_view::npos) {
      return true;
    }
    if (line.find("RAG") != std::string_view::npos ||
        line.find("MCP") != std::string_view::npos) {
      return line.find("Pattern") != std::string_view::npos;
    }
    return false;
  }

  static std::string clean_toc_line(std::string text) {
    for (auto& ch : text) {
      if (ch == '\t' || ch == '\r' || ch == '\n') {
        ch = ' ';
      }
    }
    while (text.find("  ") != std::string::npos) {
      text.replace(text.find("  "), 2, " ");
    }
    const auto overview = text.find("Pattern Overview");
    if (overview != std::string::npos) {
      text.resize(overview + std::string("Pattern").size());
    }
    while (!text.empty() && std::isdigit(static_cast<unsigned char>(text.back()))) {
      text.pop_back();
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
      text.pop_back();
    }
    return text_detail::trim_copy(text);
  }

  static std::string infer_plain_text_section(
    const knowledge_document& document,
    std::size_t offset) {
    if (!is_tika_document(document)) {
      return {};
    }

    const auto& content = document.content;
    const auto search_start = offset > 5000 ? offset - 5000 : std::size_t {};
    std::string best;
    std::size_t line_start = search_start;
    while (line_start < content.size() && line_start <= offset) {
      const auto newline = content.find('\n', line_start);
      const auto line_end = newline == std::string::npos ? content.size() : newline;
      const auto line = std::string_view(content).substr(line_start, line_end - line_start);
      if (looks_like_plain_text_section(line)) {
        best = text_detail::trim_copy(line);
      }
      if (newline == std::string::npos) {
        break;
      }
      line_start = newline + 1;
    }
    return best;
  }

  static std::vector<markdown_section> markdown_sections(const std::string& content) {
    std::vector<markdown_section> sections;
    std::size_t line_start = 0;

    while (line_start < content.size()) {
      const auto newline = content.find('\n', line_start);
      const std::size_t line_end = newline == std::string::npos ? content.size() : newline;
      std::string title;
      if (is_markdown_heading(std::string_view(content).substr(line_start, line_end - line_start),
            title)) {
        if (!sections.empty()) {
          sections.back().end = line_start;
        }
        sections.push_back({
          .start = line_start,
          .end = content.size(),
          .title = std::move(title),
        });
      }

      if (newline == std::string::npos) {
        break;
      }
      line_start = newline + 1;
    }

    return sections;
  }

  static std::vector<token_span> token_spans(
    const std::string& content,
    std::size_t range_start,
    std::size_t range_end) {
    std::vector<token_span> tokens;
    std::size_t index = range_start;
    while (index < range_end) {
      while (index < range_end &&
             std::isspace(static_cast<unsigned char>(content[index]))) {
        ++index;
      }
      if (index >= range_end) {
        break;
      }

      const auto start = index;
      while (index < range_end &&
             !std::isspace(static_cast<unsigned char>(content[index]))) {
        ++index;
      }
      tokens.push_back({
        .start = start,
        .end = index,
      });
    }
    return tokens;
  }

  static std::optional<std::size_t> open_code_fence_start_before(
    const std::string& content,
    std::size_t range_start,
    std::size_t offset) {
    bool open = false;
    std::size_t open_start = 0;
    std::size_t line_start = range_start;
    const auto end = (std::min)(offset, content.size());

    while (line_start < end) {
      const auto newline = content.find('\n', line_start);
      const std::size_t line_end =
        newline == std::string::npos ? content.size() : newline;
      if (line_start < end &&
          is_markdown_code_fence(
            std::string_view(content).substr(line_start, line_end - line_start))) {
        open = !open;
        open_start = line_start;
      }
      if (newline == std::string::npos || newline + 1 >= end) {
        break;
      }
      line_start = newline + 1;
    }

    if (open) {
      return open_start;
    }
    return std::nullopt;
  }

  static std::optional<std::size_t> closing_code_fence_end_after(
    const std::string& content,
    std::size_t offset,
    std::size_t range_end) {
    std::size_t line_start = offset;
    while (line_start < range_end) {
      const auto newline = content.find('\n', line_start);
      const std::size_t line_end =
        newline == std::string::npos ? range_end : (std::min)(newline, range_end);
      if (is_markdown_code_fence(
            std::string_view(content).substr(line_start, line_end - line_start))) {
        return newline == std::string::npos ? line_end : (std::min)(newline + 1, range_end);
      }
      if (newline == std::string::npos || newline + 1 >= range_end) {
        break;
      }
      line_start = newline + 1;
    }
    return std::nullopt;
  }

  std::size_t choose_chunk_end(
    const std::string& content,
    std::size_t start,
    std::size_t range_end) const {
    const std::size_t hard_end = (std::min)(range_end, start + policy_.max_chars);
    if (!policy_.prefer_paragraph_boundaries || hard_end == range_end) {
      return hard_end;
    }

    const std::size_t minimum_end = start + (policy_.max_chars / 2);
    if (policy_.protect_markdown_code_fences) {
      const auto open_fence = open_code_fence_start_before(content, start, hard_end);
      if (open_fence && *open_fence >= minimum_end) {
        return *open_fence;
      }
      if (open_fence) {
        const auto closing_fence = closing_code_fence_end_after(content, hard_end, range_end);
        if (closing_fence) {
          return *closing_fence;
        }
      }
    }

    const auto paragraph = content.rfind("\n\n", hard_end);
    if (paragraph != std::string::npos && paragraph >= minimum_end) {
      return paragraph + 2;
    }

    const auto newline = content.rfind('\n', hard_end);
    if (newline != std::string::npos && newline >= minimum_end) {
      return newline + 1;
    }

    return hard_end;
  }

  std::vector<knowledge_chunk> split_markdown_sections(
    const knowledge_document& document) const {
    std::vector<knowledge_chunk> chunks;
    for (const auto& section : markdown_sections(document.content)) {
      if (section.end <= section.start) {
        continue;
      }

      std::map<std::string, std::string> metadata = document.metadata;
      metadata["section"] = section.title;
      append_range_chunks(document, section.start, section.end, std::move(metadata), chunks);
    }
    return chunks;
  }

  std::vector<knowledge_chunk> split_code_symbols(const knowledge_document& document) const {
    std::vector<std::size_t> starts { 0 };
    std::size_t line_start = 0;
    while (line_start < document.content.size()) {
      const auto newline = document.content.find('\n', line_start);
      const auto line_end = newline == std::string::npos ? document.content.size() : newline;
      if (line_start != 0 &&
          looks_like_code_symbol_start(
            std::string_view(document.content).substr(line_start, line_end - line_start))) {
        starts.push_back(line_start);
      }
      if (newline == std::string::npos) {
        break;
      }
      line_start = newline + 1;
    }

    if (starts.size() <= 1) {
      return {};
    }
    starts.push_back(document.content.size());

    std::vector<knowledge_chunk> chunks;
    for (std::size_t index = 0; index + 1 < starts.size(); ++index) {
      const auto start = starts[index];
      const auto end = starts[index + 1];
      if (start >= end) {
        continue;
      }
      auto metadata = document.metadata;
      metadata["chunking"] = "code_symbol";
      append_range_chunks(document, start, end, std::move(metadata), chunks);
    }
    return chunks;
  }

  void append_range_chunks(
    const knowledge_document& document,
    std::size_t range_start,
    std::size_t range_end,
    std::map<std::string, std::string> metadata,
    std::vector<knowledge_chunk>& chunks) const {
    if (policy_.max_tokens != 0) {
      append_token_range_chunks(document, range_start, range_end, std::move(metadata), chunks);
      return;
    }

    std::size_t start = range_start;
    while (start < range_end) {
      const std::size_t end = choose_chunk_end(document.content, start, range_end);

      knowledge_chunk chunk;
      chunk.document_id = document.id;
      chunk.id = document.id + "#chunk-" + std::to_string(chunks.size() + 1);
      chunk.title = document.title;
      chunk.content = document.content.substr(start, end - start);
      chunk.start_offset = start;
      chunk.end_offset = end;
      chunk.start_line = line_for_offset(document.content, start);
      chunk.end_line = line_for_offset(document.content, end);
      chunk.source_uri = document.source_uri;
      chunk.metadata = metadata.empty() ? document.metadata : metadata;
      apply_plain_text_metadata(document, chunk);
      chunks.push_back(std::move(chunk));

      if (end == range_end) {
        break;
      }
      start = end - policy_.overlap_chars;
    }
  }

  void append_token_range_chunks(
    const knowledge_document& document,
    std::size_t range_start,
    std::size_t range_end,
    std::map<std::string, std::string> metadata,
    std::vector<knowledge_chunk>& chunks) const {
    const auto tokens = token_spans(document.content, range_start, range_end);
    if (tokens.empty()) {
      return;
    }

    std::size_t token_start = 0;
    while (token_start < tokens.size()) {
      const auto token_end = (std::min)(tokens.size(), token_start + policy_.max_tokens);
      const auto start = tokens[token_start].start;
      const auto end = tokens[token_end - 1].end;

      knowledge_chunk chunk;
      chunk.document_id = document.id;
      chunk.id = document.id + "#chunk-" + std::to_string(chunks.size() + 1);
      chunk.title = document.title;
      chunk.content = document.content.substr(start, end - start);
      chunk.start_offset = start;
      chunk.end_offset = end;
      chunk.start_line = line_for_offset(document.content, start);
      chunk.end_line = line_for_offset(document.content, end);
      chunk.source_uri = document.source_uri;
      chunk.metadata = metadata.empty() ? document.metadata : metadata;
      chunk.metadata["chunking"] = "token";
      apply_plain_text_metadata(document, chunk);
      chunks.push_back(std::move(chunk));

      if (token_end == tokens.size()) {
        break;
      }

      token_start = token_end - policy_.overlap_tokens;
    }
  }

  chunking_policy policy_;

  static std::vector<std::string> extract_toc_entries(const knowledge_document& document) {
    std::vector<std::string> toc_lines;
    std::size_t line_start = 0;
    while (line_start < document.content.size() && toc_lines.size() < 80) {
      const auto newline = document.content.find('\n', line_start);
      const auto line_end = newline == std::string::npos ? document.content.size() : newline;
      const auto line = std::string_view(document.content).substr(line_start, line_end - line_start);
      if (looks_like_toc_pattern_line(line)) {
        auto cleaned = clean_toc_line(text_detail::trim_copy(line));
        if (!cleaned.empty() &&
            std::find(toc_lines.begin(), toc_lines.end(), cleaned) == toc_lines.end()) {
          toc_lines.push_back(std::move(cleaned));
        }
      }
      if (newline == std::string::npos) {
        break;
      }
      line_start = newline + 1;
    }
    return toc_lines;
  }

  static document_summary collect_document_summary_lines(
    const knowledge_document& document,
    std::size_t max_chars) {
    std::ostringstream output;
    if (!document.title.empty()) {
      output << "Title: " << document.title << "\n";
    }
    if (!document.source_uri.empty()) {
      output << "Source: " << document.source_uri << "\n";
    }
    if (const auto summary = document.metadata.find("summary");
        summary != document.metadata.end() && !summary->second.empty()) {
      output << "LLM summary:\n" << summary->second << "\n";
    }
    if (const auto toc = document.metadata.find("toc");
        toc != document.metadata.end() && !toc->second.empty()) {
      output << "Table of contents:\n" << toc->second << "\n";
    }

    output << "Overview:\n";
    const auto overview_size = (std::min)(document.content.size(), std::size_t { 1200 });
    output << document.content.substr(0, overview_size) << "\n";

    std::size_t collected = 0;
    bool wrote_heading = false;
    std::size_t line_start = 0;
    while (line_start < document.content.size() && output.tellp() < static_cast<std::streampos>(max_chars)) {
      const auto newline = document.content.find('\n', line_start);
      const auto line_end = newline == std::string::npos ? document.content.size() : newline;
      const auto line = std::string_view(document.content).substr(line_start, line_end - line_start);
      if (looks_like_document_summary_line(line)) {
        if (!wrote_heading) {
          output << "\nLikely sections and patterns:\n";
          wrote_heading = true;
        }
        output << "- " << text_detail::trim_copy(line) << "\n";
        ++collected;
        if (collected >= 80) {
          break;
        }
      }
      if (newline == std::string::npos) {
        break;
      }
      line_start = newline + 1;
    }

    auto toc_lines = extract_toc_entries(document);
    if (!toc_lines.empty() && output.tellp() < static_cast<std::streampos>(max_chars)) {
      output << "\nExtracted pattern table of contents:\n";
      for (const auto& line : toc_lines) {
        output << "- " << line << "\n";
      }
    }

    auto summary = output.str();
    if (summary.size() > max_chars) {
      summary.resize(max_chars);
      if (summary.size() > 3) {
        summary.resize(summary.size() - 3);
        summary += "...";
      }
    }
    return {
      .content = std::move(summary),
      .toc_entries = std::move(toc_lines),
    };
  }

  void prepend_document_summary_chunk(
    const knowledge_document& document,
    std::vector<knowledge_chunk>& chunks) const {
    if (!policy_.include_document_summary_chunk || chunks.empty()) {
      return;
    }

    auto collected = collect_document_summary_lines(document, policy_.document_summary_chars);
    if (collected.content.empty()) {
      return;
    }

    knowledge_chunk summary;
    summary.id = document.id + "#summary";
    summary.document_id = document.id;
    summary.title = document.title;
    summary.content = std::move(collected.content);
    summary.start_offset = 0;
    summary.end_offset = 0;
    summary.start_line = 1;
    summary.end_line = 1;
    summary.source_uri = document.source_uri;
    summary.metadata = document.metadata;
    summary.metadata["chunking"] = "document_summary";
    if (!collected.toc_entries.empty()) {
      summary.metadata["toc_entries"] = nlohmann::json(collected.toc_entries).dump();
    }
    chunks.insert(chunks.begin(), std::move(summary));
  }

  static void apply_plain_text_metadata(
    const knowledge_document& document,
    knowledge_chunk& chunk) {
    if (has_page_breaks(document)) {
      chunk.metadata["page_start"] = std::to_string(page_for_offset(document.content, chunk.start_offset));
      chunk.metadata["page_end"] = std::to_string(page_for_offset(document.content, chunk.end_offset));
    }
    if (!chunk.metadata.contains("section")) {
      const auto section = infer_plain_text_section(document, chunk.start_offset);
      if (!section.empty()) {
        chunk.metadata["section"] = section;
      }
    }
  }
};

} // namespace wuwe::agent::knowledge

#endif // WUWE_AGENT_KNOWLEDGE_SPLITTER_HPP
