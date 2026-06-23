#include "restricted_process_runtime_staging.hpp"

#include <algorithm>
#include <cwctype>
#include <utility>

namespace wuwe::agent::execution::detail {
namespace {

restricted_python_runtime_staging_result make_result(
  const restricted_python_runtime_staging_request& request,
  restricted_python_runtime_staging_status status,
  std::error_code error = {},
  std::string detail = {}) {
  return {
    .status = status,
    .source_home = request.source_python.parent_path(),
    .destination_home = request.destination_home,
    .python_executable =
      request.destination_home / request.source_python.filename(),
    .system_error = error,
    .detail = std::move(detail),
  };
}

std::wstring lowercase(std::wstring text) {
  std::transform(text.begin(), text.end(), text.begin(), [](wchar_t ch) {
    return static_cast<wchar_t>(std::towlower(ch));
  });
  return text;
}

bool is_windows_runtime_dll(const std::filesystem::path& path) {
  const auto extension = lowercase(path.extension().wstring());
  if (extension != L".dll") {
    return false;
  }

  const auto filename = lowercase(path.filename().wstring());
  return filename.rfind(L"python", 0) == 0 ||
         filename.rfind(L"vcruntime", 0) == 0;
}

bool copy_required_file(
  const std::filesystem::path& source,
  const std::filesystem::path& destination,
  restricted_python_runtime_staging_result& result) {
  std::error_code error;
  std::filesystem::create_directories(destination.parent_path(), error);
  if (error) {
    result.status =
      restricted_python_runtime_staging_status::create_destination_failed;
    result.system_error = error;
    result.detail = destination.parent_path().string();
    return false;
  }

  const auto copied = std::filesystem::copy_file(
    source,
    destination,
    std::filesystem::copy_options::overwrite_existing,
    error);
  if (error || !copied) {
    result.status = restricted_python_runtime_staging_status::copy_failed;
    result.system_error = error;
    result.detail = source.string();
    return false;
  }
  result.copied_files.push_back(destination);
  return true;
}

bool copy_directory_tree(
  const std::filesystem::path& source,
  const std::filesystem::path& destination,
  restricted_python_runtime_staging_result& result) {
  std::error_code error;
  if (!std::filesystem::exists(source, error)) {
    result.status = restricted_python_runtime_staging_status::source_lib_missing;
    result.system_error = error;
    result.detail = source.string();
    return false;
  }

  std::filesystem::create_directories(destination, error);
  if (error) {
    result.status =
      restricted_python_runtime_staging_status::create_destination_failed;
    result.system_error = error;
    result.detail = destination.string();
    return false;
  }

  std::filesystem::recursive_directory_iterator it(source, error);
  if (error) {
    result.status = restricted_python_runtime_staging_status::copy_failed;
    result.system_error = error;
    result.detail = source.string();
    return false;
  }

  for (std::filesystem::recursive_directory_iterator end;
       it != end;
       it.increment(error)) {
    if (error) {
      result.status = restricted_python_runtime_staging_status::copy_failed;
      result.system_error = error;
      result.detail = source.string();
      return false;
    }

    const auto relative = std::filesystem::relative(it->path(), source, error);
    if (error) {
      result.status = restricted_python_runtime_staging_status::copy_failed;
      result.system_error = error;
      result.detail = it->path().string();
      return false;
    }

    const auto target = destination / relative;
    if (it->is_directory(error)) {
      std::filesystem::create_directories(target, error);
      if (error) {
        result.status =
          restricted_python_runtime_staging_status::create_destination_failed;
        result.system_error = error;
        result.detail = target.string();
        return false;
      }
      continue;
    }
    if (error) {
      result.status = restricted_python_runtime_staging_status::copy_failed;
      result.system_error = error;
      result.detail = it->path().string();
      return false;
    }

    if (it->is_regular_file(error)) {
      if (!copy_required_file(it->path(), target, result)) {
        return false;
      }
    }
    if (error) {
      result.status = restricted_python_runtime_staging_status::copy_failed;
      result.system_error = error;
      result.detail = it->path().string();
      return false;
    }
  }
  return true;
}

} // namespace

const char* to_string(restricted_python_runtime_staging_status status) noexcept {
  switch (status) {
    case restricted_python_runtime_staging_status::ok:
      return "ok";
    case restricted_python_runtime_staging_status::unsupported_platform:
      return "unsupported_platform";
    case restricted_python_runtime_staging_status::empty_source_python:
      return "empty_source_python";
    case restricted_python_runtime_staging_status::empty_destination_home:
      return "empty_destination_home";
    case restricted_python_runtime_staging_status::source_python_not_found:
      return "source_python_not_found";
    case restricted_python_runtime_staging_status::source_python_not_regular_file:
      return "source_python_not_regular_file";
    case restricted_python_runtime_staging_status::source_home_missing:
      return "source_home_missing";
    case restricted_python_runtime_staging_status::source_lib_missing:
      return "source_lib_missing";
    case restricted_python_runtime_staging_status::create_destination_failed:
      return "create_destination_failed";
    case restricted_python_runtime_staging_status::copy_failed:
      return "copy_failed";
  }
  return "unknown";
}

restricted_python_runtime_staging_result
stage_minimal_python_runtime_for_restricted_process(
  const restricted_python_runtime_staging_request& request) {
  if (request.source_python.empty()) {
    return make_result(
      request,
      restricted_python_runtime_staging_status::empty_source_python);
  }
  if (request.destination_home.empty()) {
    return make_result(
      request,
      restricted_python_runtime_staging_status::empty_destination_home);
  }

#ifndef _WIN32
  return make_result(
    request,
    restricted_python_runtime_staging_status::unsupported_platform);
#else
  std::error_code error;
  if (!std::filesystem::exists(request.source_python, error)) {
    return make_result(
      request,
      restricted_python_runtime_staging_status::source_python_not_found,
      error,
      request.source_python.string());
  }
  if (!std::filesystem::is_regular_file(request.source_python, error)) {
    return make_result(
      request,
      restricted_python_runtime_staging_status::source_python_not_regular_file,
      error,
      request.source_python.string());
  }

  const auto source_home = request.source_python.parent_path();
  if (source_home.empty() || !std::filesystem::exists(source_home, error)) {
    return make_result(
      request,
      restricted_python_runtime_staging_status::source_home_missing,
      error,
      source_home.string());
  }

  auto result = make_result(
    request,
    restricted_python_runtime_staging_status::ok);

  if (request.replace_existing) {
    std::filesystem::remove_all(request.destination_home, error);
    if (error) {
      result.status =
        restricted_python_runtime_staging_status::create_destination_failed;
      result.system_error = error;
      result.detail = request.destination_home.string();
      return result;
    }
  }

  std::filesystem::create_directories(request.destination_home, error);
  if (error) {
    result.status =
      restricted_python_runtime_staging_status::create_destination_failed;
    result.system_error = error;
    result.detail = request.destination_home.string();
    return result;
  }

  if (!copy_required_file(
        request.source_python,
        result.python_executable,
        result)) {
    return result;
  }

  std::filesystem::directory_iterator it(source_home, error);
  if (error) {
    result.status = restricted_python_runtime_staging_status::copy_failed;
    result.system_error = error;
    result.detail = source_home.string();
    return result;
  }

  for (std::filesystem::directory_iterator end; it != end; it.increment(error)) {
    if (error) {
      result.status = restricted_python_runtime_staging_status::copy_failed;
      result.system_error = error;
      result.detail = source_home.string();
      return result;
    }

    if (it->is_regular_file(error) && is_windows_runtime_dll(it->path())) {
      if (!copy_required_file(
            it->path(),
            request.destination_home / it->path().filename(),
            result)) {
        return result;
      }
    }
    if (error) {
      result.status = restricted_python_runtime_staging_status::copy_failed;
      result.system_error = error;
      result.detail = it->path().string();
      return result;
    }
  }

  const auto source_lib = source_home / "Lib";
  if (!copy_directory_tree(
        source_lib / "encodings",
        request.destination_home / "Lib" / "encodings",
        result)) {
    return result;
  }

  const auto codecs = source_lib / "codecs.py";
  if (std::filesystem::exists(codecs, error)) {
    if (!copy_required_file(
          codecs,
          request.destination_home / "Lib" / "codecs.py",
          result)) {
      return result;
    }
  }
  if (error) {
    result.status = restricted_python_runtime_staging_status::copy_failed;
    result.system_error = error;
    result.detail = codecs.string();
    return result;
  }

  const auto os = source_lib / "os.py";
  if (std::filesystem::exists(os, error)) {
    if (!copy_required_file(
          os,
          request.destination_home / "Lib" / "os.py",
          result)) {
      return result;
    }
  }
  if (error) {
    result.status = restricted_python_runtime_staging_status::copy_failed;
    result.system_error = error;
    result.detail = os.string();
    return result;
  }

  return result;
#endif
}

} // namespace wuwe::agent::execution::detail
