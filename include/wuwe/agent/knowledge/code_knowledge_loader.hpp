#ifndef WUWE_AGENT_KNOWLEDGE_CODE_KNOWLEDGE_LOADER_HPP
#define WUWE_AGENT_KNOWLEDGE_CODE_KNOWLEDGE_LOADER_HPP

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <wuwe/agent/knowledge/file_knowledge_loader.hpp>

namespace wuwe::agent::knowledge {

struct code_knowledge_loader_options {
  std::set<std::string> extensions {
    ".c", ".cc", ".cpp", ".cxx", ".cs", ".go", ".h", ".hh", ".hpp", ".hxx",
    ".java", ".js", ".jsx", ".py", ".rs", ".ts", ".tsx"
  };
  std::set<std::string> excluded_directories {
    ".git", ".hg", ".svn", "build", "build-vcpkg", "cmake-build-debug",
    "dist", "node_modules", "out", "target", "vendor"
  };
  std::map<std::string, std::string> metadata;
};

class code_knowledge_loader {
public:
  explicit code_knowledge_loader(file_knowledge_loader file_loader = {})
      : file_loader_(std::move(file_loader)) {
  }

  std::vector<knowledge_document> load_repository(
    const std::filesystem::path& root,
    code_knowledge_loader_options options = {}) const {
    if (!std::filesystem::exists(root)) {
      throw std::runtime_error("code repository does not exist: " + root.string());
    }
    if (!std::filesystem::is_directory(root)) {
      throw std::runtime_error("code repository path is not a directory: " + root.string());
    }

    std::vector<std::filesystem::path> paths;
    std::filesystem::recursive_directory_iterator it(root);
    const std::filesystem::recursive_directory_iterator end;
    while (it != end) {
      const auto& entry = *it;
      if (entry.is_directory() &&
          options.excluded_directories.contains(entry.path().filename().string())) {
        it.disable_recursion_pending();
        ++it;
        continue;
      }
      append_candidate(entry, options, paths);
      ++it;
    }

    std::sort(paths.begin(), paths.end());

    std::vector<knowledge_document> documents;
    documents.reserve(paths.size());
    for (const auto& path : paths) {
      auto metadata = options.metadata;
      metadata["relative_path"] = std::filesystem::relative(path, root).generic_string();
      metadata["language"] = language_for_extension(lowercase_extension(path));

      auto id = metadata["relative_path"];
      for (auto& ch : id) {
        if (ch == '\\' || ch == '/' || ch == ' ' || ch == ':') {
          ch = '-';
        }
      }

      documents.push_back(file_loader_.load(path, {
        .id = std::move(id),
        .metadata = std::move(metadata),
      }));
    }
    return documents;
  }

private:
  static void append_candidate(
    const std::filesystem::directory_entry& entry,
    const code_knowledge_loader_options& options,
    std::vector<std::filesystem::path>& paths) {
    if (!entry.is_regular_file()) {
      return;
    }
    const auto extension = lowercase_extension(entry.path());
    if (!options.extensions.empty() && !options.extensions.contains(extension)) {
      return;
    }
    paths.push_back(entry.path());
  }

  static std::string lowercase_extension(const std::filesystem::path& path) {
    auto extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
      return static_cast<char>(std::tolower(ch));
    });
    return extension;
  }

  static std::string language_for_extension(const std::string& extension) {
    if (extension == ".c" || extension == ".h") {
      return "c";
    }
    if (extension == ".cc" || extension == ".cpp" || extension == ".cxx" ||
        extension == ".hh" || extension == ".hpp" || extension == ".hxx") {
      return "cpp";
    }
    if (extension == ".cs") {
      return "csharp";
    }
    if (extension == ".go") {
      return "go";
    }
    if (extension == ".java") {
      return "java";
    }
    if (extension == ".js" || extension == ".jsx") {
      return "javascript";
    }
    if (extension == ".py") {
      return "python";
    }
    if (extension == ".rs") {
      return "rust";
    }
    if (extension == ".ts" || extension == ".tsx") {
      return "typescript";
    }
    return "code";
  }

  file_knowledge_loader file_loader_;
};

} // namespace wuwe::agent::knowledge

#endif // WUWE_AGENT_KNOWLEDGE_CODE_KNOWLEDGE_LOADER_HPP
