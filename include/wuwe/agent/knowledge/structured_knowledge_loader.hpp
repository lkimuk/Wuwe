#ifndef WUWE_AGENT_KNOWLEDGE_STRUCTURED_KNOWLEDGE_LOADER_HPP
#define WUWE_AGENT_KNOWLEDGE_STRUCTURED_KNOWLEDGE_LOADER_HPP

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <wuwe/agent/knowledge/file_knowledge_loader.hpp>
#include <wuwe/agent/knowledge/knowledge_hash.hpp>
#include <wuwe/agent/knowledge/knowledge_record.hpp>

namespace wuwe::agent::knowledge {

struct structured_knowledge_loader_options {
  std::string id;
  std::string title;
  std::string source_uri;
  std::map<std::string, std::string> metadata;
  std::size_t max_json_scalar_length { 2000 };
};

class structured_knowledge_loader {
public:
  knowledge_document load_csv(
    const std::filesystem::path& path,
    structured_knowledge_loader_options options = {}) const {
    const auto text = read_file(path);
    auto rows = parse_csv(text);

    std::ostringstream content;
    std::size_t data_rows = 0;
    if (!rows.empty()) {
      const auto headers = rows.front();
      for (std::size_t row_index = 1; row_index < rows.size(); ++row_index) {
        if (row_is_empty(rows[row_index])) {
          continue;
        }
        ++data_rows;
        content << "Row " << row_index << ":\n";
        for (std::size_t column = 0; column < rows[row_index].size(); ++column) {
          const auto name = column < headers.size() && !headers[column].empty()
                              ? headers[column]
                              : "column_" + std::to_string(column + 1);
          content << "- " << name << ": " << rows[row_index][column] << '\n';
        }
        content << '\n';
      }
    }

    auto document = make_document(path, std::move(options), content.str());
    document.metadata["content_type"] = "text/csv";
    document.metadata["structured_as"] = "csv_rows";
    document.metadata["row_count"] = std::to_string(data_rows);
    return document;
  }

  knowledge_document load_json(
    const std::filesystem::path& path,
    structured_knowledge_loader_options options = {}) const {
    const auto text = read_file(path);
    const auto json = nlohmann::json::parse(text);

    std::ostringstream content;
    flatten_json(json, "$", content, options.max_json_scalar_length);

    auto document = make_document(path, std::move(options), content.str());
    document.metadata["content_type"] = "application/json";
    document.metadata["structured_as"] = "json_paths";
    return document;
  }

  knowledge_document load_openapi_json(
    const std::filesystem::path& path,
    structured_knowledge_loader_options options = {}) const {
    const auto text = read_file(path);
    const auto json = nlohmann::json::parse(text);

    std::ostringstream content;
    content << "OpenAPI: " << json.value("openapi", json.value("swagger", "")) << '\n';
    if (json.contains("info") && json["info"].is_object()) {
      content << "Title: " << json["info"].value("title", "") << '\n';
      content << "Version: " << json["info"].value("version", "") << "\n\n";
    }

    if (json.contains("paths") && json["paths"].is_object()) {
      for (const auto& [route, route_item] : json["paths"].items()) {
        if (!route_item.is_object()) {
          continue;
        }
        for (const auto& [method, operation] : route_item.items()) {
          if (!operation.is_object() || !is_http_method(method)) {
            continue;
          }
          content << method << " " << route << '\n';
          content << "summary: " << operation.value("summary", "") << '\n';
          content << "operationId: " << operation.value("operationId", "") << '\n';
          if (operation.contains("tags") && operation["tags"].is_array()) {
            content << "tags: ";
            bool first = true;
            for (const auto& tag : operation["tags"]) {
              if (!first) {
                content << ", ";
              }
              content << tag.get<std::string>();
              first = false;
            }
            content << '\n';
          }
          content << '\n';
        }
      }
    }

    auto document = make_document(path, std::move(options), content.str());
    document.metadata["content_type"] = "application/vnd.oai.openapi+json";
    document.metadata["structured_as"] = "openapi_operations";
    return document;
  }

private:
  static std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
      throw std::runtime_error("failed to open structured knowledge file: " + path.string());
    }
    std::ostringstream content;
    content << input.rdbuf();
    return content.str();
  }

  static knowledge_document make_document(
    const std::filesystem::path& path,
    structured_knowledge_loader_options options,
    std::string content) {
    knowledge_document document;
    document.id = options.id.empty() ? file_knowledge_loader::default_id(path) : std::move(options.id);
    document.title = options.title.empty() ? path.stem().string() : std::move(options.title);
    document.source_uri =
      options.source_uri.empty() ? path.generic_string() : std::move(options.source_uri);
    document.metadata = std::move(options.metadata);
    document.content = std::move(content);
    if (!path.extension().empty()) {
      document.metadata.try_emplace("extension", path.extension().string());
    }
    document.metadata["content_hash"] = stable_hash(document.content);
    return document;
  }

  static std::vector<std::vector<std::string>> parse_csv(std::string_view text) {
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> row;
    std::string cell;
    bool quoted = false;

    for (std::size_t index = 0; index < text.size(); ++index) {
      const auto ch = text[index];
      if (quoted) {
        if (ch == '"' && index + 1 < text.size() && text[index + 1] == '"') {
          cell.push_back('"');
          ++index;
        }
        else if (ch == '"') {
          quoted = false;
        }
        else {
          cell.push_back(ch);
        }
        continue;
      }

      if (ch == '"') {
        quoted = true;
      }
      else if (ch == ',') {
        row.push_back(std::move(cell));
        cell.clear();
      }
      else if (ch == '\n') {
        row.push_back(std::move(cell));
        cell.clear();
        rows.push_back(std::move(row));
        row.clear();
      }
      else if (ch != '\r') {
        cell.push_back(ch);
      }
    }
    row.push_back(std::move(cell));
    if (!row.empty()) {
      rows.push_back(std::move(row));
    }
    return rows;
  }

  static bool row_is_empty(const std::vector<std::string>& row) {
    for (const auto& cell : row) {
      if (!cell.empty()) {
        return false;
      }
    }
    return true;
  }

  static void flatten_json(
    const nlohmann::json& value,
    const std::string& path,
    std::ostringstream& output,
    std::size_t max_scalar_length) {
    if (value.is_object()) {
      for (const auto& [key, item] : value.items()) {
        flatten_json(item, path + "." + key, output, max_scalar_length);
      }
      return;
    }
    if (value.is_array()) {
      for (std::size_t index = 0; index < value.size(); ++index) {
        flatten_json(value[index], path + "[" + std::to_string(index) + "]", output, max_scalar_length);
      }
      return;
    }

    auto scalar = value.is_string() ? value.get<std::string>() : value.dump();
    if (scalar.size() > max_scalar_length) {
      scalar.resize(max_scalar_length);
    }
    output << path << ": " << scalar << '\n';
  }

  static bool is_http_method(const std::string& value) {
    return value == "get" || value == "put" || value == "post" ||
           value == "delete" || value == "patch" || value == "head" ||
           value == "options" || value == "trace";
  }
};

} // namespace wuwe::agent::knowledge

#endif // WUWE_AGENT_KNOWLEDGE_STRUCTURED_KNOWLEDGE_LOADER_HPP
