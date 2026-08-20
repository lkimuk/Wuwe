#include <wuwe/runtime_host/plugin_package.hpp>

#include <array>
#include <cstdint>
#include <fstream>
#include <limits>
#include <span>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include <wuwe/agent/core/filesystem.hpp>
#include <wuwe/agent/execution/path_policy.hpp>
#include <wuwe/common/sha256.hpp>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace wuwe::runtime_host {
namespace {

constexpr std::uintmax_t maximum_manifest_bytes = 1024 * 1024;

std::string path_text(const std::filesystem::path& path) {
  return agent::core::filesystem_path_to_utf8(path);
}

[[noreturn]] void fail(plugin_package_error_code code, std::string message) {
  throw plugin_package_error(code, "plugin package: " + std::move(message));
}

bool is_reparse_point(const std::filesystem::path& path) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error == std::errc::no_such_file_or_directory)
    return false;
  if (error)
    fail(plugin_package_error_code::filesystem_error,
      "unable to inspect path '" + path_text(path) + "'");
  if (!std::filesystem::exists(status))
    return false;
  if (std::filesystem::is_symlink(status))
    return true;
#if defined(_WIN32)
  const auto attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES)
    fail(plugin_package_error_code::filesystem_error,
      "unable to inspect path attributes for '" + path_text(path) + "'");
  return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
  return false;
#endif
}

void require_regular_file(const std::filesystem::path& path, std::string_view purpose,
  plugin_package_error_code unavailable_code) {
  if (is_reparse_point(path))
    fail(plugin_package_error_code::unsafe_path,
      std::string(purpose) + " must not be a symbolic link or reparse point");
  std::error_code error;
  const auto status = std::filesystem::status(path, error);
  if (error == std::errc::no_such_file_or_directory)
    fail(unavailable_code, std::string(purpose) + " is unavailable: '" + path_text(path) + "'");
  if (error)
    fail(plugin_package_error_code::filesystem_error,
      "unable to inspect " + std::string(purpose) + ": '" + path_text(path) + "'");
  if (!std::filesystem::exists(status))
    fail(unavailable_code, std::string(purpose) + " is unavailable: '" + path_text(path) + "'");
  if (!std::filesystem::is_regular_file(status))
    fail(plugin_package_error_code::unsupported_file,
      std::string(purpose) + " is not a regular file: '" + path_text(path) + "'");
  const auto links = std::filesystem::hard_link_count(path, error);
  if (error)
    fail(plugin_package_error_code::filesystem_error,
      "unable to inspect hard links for '" + path_text(path) + "'");
  if (links > 1)
    fail(plugin_package_error_code::unsafe_path, std::string(purpose) + " must not be a hard link");
}

void reject_reparse_components(
  const std::filesystem::path& root, const std::filesystem::path& target) {
  if (!agent::execution::path_is_within_root(target, root))
    fail(plugin_package_error_code::unsafe_path, "path escapes the plugin package root");
  auto current = root;
  if (is_reparse_point(current))
    fail(plugin_package_error_code::unsafe_path,
      "package root must not be a symbolic link or reparse point");
  auto target_it = target.begin();
  for (auto root_it = root.begin(); root_it != root.end(); ++root_it)
    ++target_it;
  for (; target_it != target.end(); ++target_it) {
    current /= *target_it;
    if (is_reparse_point(current))
      fail(plugin_package_error_code::unsafe_path,
        "package contains a symbolic link or reparse point: '" + path_text(current) + "'");
  }
}

struct file_digest {
  std::string content;
  std::string sha256;
  std::uintmax_t size {};
};

file_digest read_and_hash_file(
  const std::filesystem::path& path, std::uintmax_t maximum_size, bool retain_content) {
  const auto purpose = retain_content ? std::string_view("plugin manifest")
                                      : std::string_view("plugin entrypoint");
  const auto unavailable = retain_content ? plugin_package_error_code::manifest_unavailable
                                          : plugin_package_error_code::entrypoint_unavailable;
  require_regular_file(path, purpose, unavailable);
  std::error_code error;
  const auto initial_size = std::filesystem::file_size(path, error);
  if (error)
    fail(plugin_package_error_code::filesystem_error,
      "unable to determine file size for '" + path_text(path) + "'");
  if (initial_size > maximum_size || initial_size > std::numeric_limits<std::size_t>::max())
    fail(retain_content ? plugin_package_error_code::manifest_too_large
                        : plugin_package_error_code::unsupported_file,
      retain_content ? "plugin manifest exceeds the size limit"
                     : "plugin entrypoint is too large to validate");
  const auto initial_write_time = std::filesystem::last_write_time(path, error);
  if (error)
    fail(plugin_package_error_code::filesystem_error,
      "unable to inspect modification time for '" + path_text(path) + "'");

  std::ifstream input(path, std::ios::binary);
  if (!input)
    fail(plugin_package_error_code::filesystem_error,
      "unable to open file '" + path_text(path) + "'");
  file_digest result;
  result.size = initial_size;
  if (retain_content)
    result.content.reserve(static_cast<std::size_t>(initial_size));
  common::sha256 hash;
  std::array<char, 64 * 1024> buffer {};
  std::uintmax_t observed_size = 0;
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = input.gcount();
    if (count <= 0)
      continue;
    const auto size = static_cast<std::size_t>(count);
    if (observed_size > maximum_size - size)
      fail(retain_content ? plugin_package_error_code::manifest_too_large
                          : plugin_package_error_code::file_changed,
        retain_content ? "plugin manifest exceeds the size limit"
                       : "plugin entrypoint changed while being validated");
    observed_size += size;
    hash.update(std::string_view(buffer.data(), size));
    if (retain_content)
      result.content.append(buffer.data(), size);
  }
  if (!input.eof())
    fail(plugin_package_error_code::filesystem_error,
      "failed while reading file '" + path_text(path) + "'");
  if (observed_size != initial_size)
    fail(plugin_package_error_code::file_changed,
      "file changed while being validated: '" + path_text(path) + "'");
  require_regular_file(path, purpose, unavailable);
  const auto final_size = std::filesystem::file_size(path, error);
  if (error || final_size != initial_size)
    fail(plugin_package_error_code::file_changed,
      "file changed while being validated: '" + path_text(path) + "'");
  const auto final_write_time = std::filesystem::last_write_time(path, error);
  if (error || final_write_time != initial_write_time)
    fail(plugin_package_error_code::file_changed,
      "file changed while being validated: '" + path_text(path) + "'");
  result.sha256 = hash.hex_digest();
  return result;
}

void hash_u64(common::sha256& hash, std::uint64_t value) {
  std::array<std::uint8_t, 8> bytes {};
  for (std::size_t index = 0; index < bytes.size(); ++index)
    bytes[bytes.size() - 1 - index] = static_cast<std::uint8_t>(value >> (index * 8U));
  hash.update(std::span<const std::uint8_t>(bytes));
}

void hash_field(common::sha256& hash, std::string_view value) {
  hash_u64(hash, static_cast<std::uint64_t>(value.size()));
  hash.update(value);
}

std::string package_digest(const file_digest& manifest, const plugin_manifest& parsed,
  const file_digest& entrypoint) {
  common::sha256 hash;
  constexpr char domain[] = "wuwe.runtime.plugin.package.v1\0";
  hash.update(std::string_view(domain, sizeof(domain) - 1));
  hash_field(hash, plugin_manifest_filename);
  hash_field(hash, manifest.content);
  hash_field(hash, parsed.entrypoint.generic_string());
  hash_u64(hash, static_cast<std::uint64_t>(entrypoint.size));
  hash_field(hash, entrypoint.sha256);
  return hash.hex_digest();
}

} // namespace

const char* to_string(plugin_package_error_code code) noexcept {
  switch (code) {
    case plugin_package_error_code::invalid_root:
      return "invalid_root";
    case plugin_package_error_code::manifest_unavailable:
      return "manifest_unavailable";
    case plugin_package_error_code::manifest_too_large:
      return "manifest_too_large";
    case plugin_package_error_code::manifest_invalid:
      return "manifest_invalid";
    case plugin_package_error_code::entrypoint_unavailable:
      return "entrypoint_unavailable";
    case plugin_package_error_code::unsafe_path:
      return "unsafe_path";
    case plugin_package_error_code::unsupported_file:
      return "unsupported_file";
    case plugin_package_error_code::file_changed:
      return "file_changed";
    case plugin_package_error_code::filesystem_error:
      return "filesystem_error";
  }
  return "unknown";
}

plugin_package_error::plugin_package_error(plugin_package_error_code code, std::string message)
    : std::runtime_error(std::move(message)), code_(code) {
}

plugin_package::plugin_package(plugin_manifest manifest, std::filesystem::path root,
  std::filesystem::path manifest_path, std::filesystem::path entrypoint_path,
  std::string content_sha256)
    : manifest_(std::move(manifest)),
      root_(std::move(root)),
      manifest_path_(std::move(manifest_path)),
      entrypoint_path_(std::move(entrypoint_path)),
      content_sha256_(std::move(content_sha256)) {
}

plugin_package_ptr load_plugin_package(const std::filesystem::path& root) {
  if (root.empty())
    fail(plugin_package_error_code::invalid_root, "package root must not be empty");
  if (is_reparse_point(root))
    fail(plugin_package_error_code::unsafe_path,
      "package root must not be a symbolic link or reparse point");
  std::error_code error;
  const auto root_status = std::filesystem::status(root, error);
  if (error == std::errc::no_such_file_or_directory || !std::filesystem::exists(root_status))
    fail(plugin_package_error_code::invalid_root,
      "package root is not a directory: '" + path_text(root) + "'");
  if (error)
    fail(plugin_package_error_code::filesystem_error,
      "unable to inspect package root: '" + path_text(root) + "'");
  if (!std::filesystem::is_directory(root_status))
    fail(plugin_package_error_code::invalid_root,
      "package root is not a directory: '" + path_text(root) + "'");
  const auto canonical_root = std::filesystem::canonical(root, error);
  if (error)
    fail(plugin_package_error_code::filesystem_error,
      "unable to canonicalize package root: '" + path_text(root) + "'");

  const auto requested_manifest = canonical_root / std::filesystem::path(plugin_manifest_filename);
  reject_reparse_components(canonical_root, requested_manifest);
  const auto manifest_path = std::filesystem::canonical(requested_manifest, error);
  if (error || !agent::execution::path_is_within_root(manifest_path, canonical_root))
    fail(plugin_package_error_code::manifest_unavailable,
      "plugin manifest is missing or escapes the package root");
  const auto manifest_file =
    read_and_hash_file(manifest_path, maximum_manifest_bytes, true);

  plugin_manifest manifest;
  try {
    manifest = plugin_manifest_from_json(nlohmann::json::parse(manifest_file.content));
  }
  catch (const std::exception& exception) {
    fail(plugin_package_error_code::manifest_invalid,
      "invalid manifest '" + path_text(manifest_path) + "': " + exception.what());
  }

  const auto requested_entrypoint = canonical_root / manifest.entrypoint;
  reject_reparse_components(canonical_root, requested_entrypoint);
  const auto entrypoint_path = std::filesystem::canonical(requested_entrypoint, error);
  if (error || !agent::execution::path_is_within_root(entrypoint_path, canonical_root))
    fail(plugin_package_error_code::entrypoint_unavailable,
      "plugin entrypoint is missing or escapes the package root");
  const auto entrypoint_file = read_and_hash_file(
    entrypoint_path, (std::numeric_limits<std::uintmax_t>::max)(), false);
  const auto content_sha256 = package_digest(manifest_file, manifest, entrypoint_file);

  return std::shared_ptr<const plugin_package>(new plugin_package(std::move(manifest),
    canonical_root,
    manifest_path,
    entrypoint_path,
    content_sha256));
}

} // namespace wuwe::runtime_host
