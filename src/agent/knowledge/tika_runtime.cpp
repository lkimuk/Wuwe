#include <wuwe/agent/knowledge/tika_runtime.hpp>

#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <wuwe/net/default_http_client.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <limits.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace wuwe::agent::knowledge {
namespace {

std::string trim_url(std::string value) {
  while (!value.empty() && value.back() == '/') {
    value.pop_back();
  }
  return value;
}

std::string default_base_url(const tika_runtime_config& config) {
  if (!config.base_url.empty()) {
    return trim_url(config.base_url);
  }
  return "http://" + config.host + ":" + std::to_string(config.port);
}

std::optional<std::filesystem::path> executable_directory() {
#ifdef _WIN32
  std::string buffer(MAX_PATH, '\0');
  DWORD size = GetModuleFileNameA(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  while (size == buffer.size()) {
    buffer.resize(buffer.size() * 2);
    size = GetModuleFileNameA(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  }
  if (size == 0) {
    return std::nullopt;
  }
  buffer.resize(size);
  return std::filesystem::path(buffer).parent_path();
#else
  std::string buffer(PATH_MAX, '\0');
  const auto size = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
  if (size <= 0) {
    return std::nullopt;
  }
  buffer.resize(static_cast<std::size_t>(size));
  return std::filesystem::path(buffer).parent_path();
#endif
}

std::vector<std::filesystem::path> runtime_candidates() {
  std::vector<std::filesystem::path> candidates;
  const auto cwd = std::filesystem::current_path();
  candidates.push_back(cwd / "runtime" / "tika");
  candidates.push_back(cwd.parent_path() / "runtime" / "tika");

  if (auto exe_dir = executable_directory()) {
    candidates.push_back(*exe_dir / "runtime" / "tika");
    candidates.push_back(exe_dir->parent_path() / "runtime" / "tika");
  }
  return candidates;
}

std::optional<std::filesystem::path> find_tika_jar(const std::filesystem::path& runtime_dir) {
  std::error_code ignored;
  if (!std::filesystem::is_directory(runtime_dir, ignored)) {
    return std::nullopt;
  }
  const auto default_jar = runtime_dir / "tika-server-standard.jar";
  if (std::filesystem::is_regular_file(default_jar, ignored)) {
    return default_jar;
  }

  for (const auto& entry : std::filesystem::directory_iterator(runtime_dir, ignored)) {
    if (!entry.is_regular_file(ignored)) {
      continue;
    }
    const auto name = entry.path().filename().string();
    if (name.rfind("tika-server", 0) == 0 && entry.path().extension() == ".jar") {
      return entry.path();
    }
  }
  return std::nullopt;
}

std::filesystem::path default_java_path(const std::filesystem::path& runtime_dir) {
#ifdef _WIN32
  const auto bundled = runtime_dir.parent_path() / "jre" / "bin" / "java.exe";
#else
  const auto bundled = runtime_dir.parent_path() / "jre" / "bin" / "java";
#endif
  std::error_code ignored;
  if (std::filesystem::is_regular_file(bundled, ignored)) {
    return bundled;
  }
  return "java";
}

#ifdef _WIN32
std::string quote_windows_arg(const std::string& arg) {
  if (arg.empty()) {
    return "\"\"";
  }
  const auto needs_quotes = arg.find_first_of(" \t\n\v\"") != std::string::npos;
  if (!needs_quotes) {
    return arg;
  }

  std::string output = "\"";
  std::size_t backslashes = 0;
  for (const char ch : arg) {
    if (ch == '\\') {
      ++backslashes;
      continue;
    }
    if (ch == '"') {
      output.append(backslashes * 2 + 1, '\\');
      output.push_back('"');
      backslashes = 0;
      continue;
    }
    output.append(backslashes, '\\');
    backslashes = 0;
    output.push_back(ch);
  }
  output.append(backslashes * 2, '\\');
  output.push_back('"');
  return output;
}

std::string build_windows_command_line(const tika_runtime_config& config) {
  std::string output = quote_windows_arg(config.java_path.string());
  output += " -jar ";
  output += quote_windows_arg(config.jar_path.string());
  output += " --host ";
  output += quote_windows_arg(config.host);
  output += " --port ";
  output += std::to_string(config.port);
  return output;
}
#endif

void wait_until_available(const tika_runtime_config& config) {
  const auto started = std::chrono::steady_clock::now();
  const auto timeout = std::chrono::milliseconds(config.startup_timeout_ms);
  const auto interval = std::chrono::milliseconds(config.poll_interval_ms);
  while (std::chrono::steady_clock::now() - started < timeout) {
    if (tika_runtime_process::service_available(config.base_url, 1000)) {
      return;
    }
    std::this_thread::sleep_for(interval);
  }
  throw std::runtime_error("Tika runtime did not become available at " + config.base_url);
}

} // namespace

struct tika_runtime_process::impl {
  tika_runtime_config config;
  bool own_process { false };

#ifdef _WIN32
  HANDLE process { nullptr };
  HANDLE thread { nullptr };
#else
  pid_t pid { -1 };
#endif

  ~impl() {
    stop();
  }

  void start_process() {
#ifdef _WIN32
    STARTUPINFOA startup {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info {};
    auto command_line = build_windows_command_line(config);
    const auto working_directory = config.runtime_dir.empty() ? std::string {} : config.runtime_dir.string();
    const auto created = CreateProcessA(
      nullptr,
      command_line.data(),
      nullptr,
      nullptr,
      FALSE,
      CREATE_NO_WINDOW,
      nullptr,
      working_directory.empty() ? nullptr : working_directory.c_str(),
      &startup,
      &info);
    if (!created) {
      throw std::runtime_error("failed to start bundled Tika runtime");
    }
    process = info.hProcess;
    thread = info.hThread;
    own_process = true;
#else
    const auto child = fork();
    if (child < 0) {
      throw std::runtime_error("failed to fork bundled Tika runtime");
    }
    if (child == 0) {
      if (!config.runtime_dir.empty() &&
          chdir(config.runtime_dir.string().c_str()) != 0) {
        _exit(127);
      }
      std::vector<std::string> args {
        config.java_path.string(),
        "-jar",
        config.jar_path.string(),
        "--host",
        config.host,
        "--port",
        std::to_string(config.port),
      };
      std::vector<char*> argv;
      argv.reserve(args.size() + 1);
      for (auto& arg : args) {
        argv.push_back(arg.data());
      }
      argv.push_back(nullptr);
      execvp(config.java_path.string().c_str(), argv.data());
      _exit(127);
    }
    pid = child;
    own_process = true;
#endif
  }

  void stop() {
    if (!own_process || !config.stop_on_destroy) {
      return;
    }
#ifdef _WIN32
    if (process) {
      const auto wait_result = WaitForSingleObject(process, 2000);
      if (wait_result == WAIT_TIMEOUT) {
        TerminateProcess(process, 1);
        WaitForSingleObject(process, 2000);
      }
    }
    if (thread) {
      CloseHandle(thread);
      thread = nullptr;
    }
    if (process) {
      CloseHandle(process);
      process = nullptr;
    }
#else
    if (pid > 0) {
      for (int i = 0; i < 20; ++i) {
        int status = 0;
        const auto result = waitpid(pid, &status, WNOHANG);
        if (result == pid) {
          pid = -1;
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      if (pid > 0) {
        kill(pid, SIGTERM);
        (void)waitpid(pid, nullptr, 0);
        pid = -1;
      }
    }
#endif
    own_process = false;
  }

  bool running() const {
#ifdef _WIN32
    if (!process) {
      return false;
    }
    DWORD exit_code = 0;
    return GetExitCodeProcess(process, &exit_code) && exit_code == STILL_ACTIVE;
#else
    return pid > 0;
#endif
  }
};

tika_runtime_process::tika_runtime_process() : impl_(std::make_unique<impl>()) {
}

tika_runtime_process::~tika_runtime_process() = default;

tika_runtime_process::tika_runtime_process(tika_runtime_process&&) noexcept = default;

tika_runtime_process& tika_runtime_process::operator=(tika_runtime_process&&) noexcept = default;

tika_runtime_discovery tika_runtime_process::discover(tika_runtime_config config) {
  config.base_url = default_base_url(config);
  if (!config.runtime_dir.empty()) {
    auto jar = find_tika_jar(config.runtime_dir);
    if (!jar) {
      return {
        .found = false,
        .config = std::move(config),
        .reason = "Tika jar not found in configured runtime_dir",
      };
    }
    config.jar_path = *jar;
    if (config.java_path.empty()) {
      config.java_path = default_java_path(config.runtime_dir);
    }
    return { .found = true, .config = std::move(config) };
  }

  for (const auto& runtime_dir : runtime_candidates()) {
    auto jar = find_tika_jar(runtime_dir);
    if (!jar) {
      continue;
    }
    config.runtime_dir = runtime_dir;
    config.jar_path = *jar;
    if (config.java_path.empty()) {
      config.java_path = default_java_path(runtime_dir);
    }
    return { .found = true, .config = std::move(config) };
  }
  return { .found = false, .config = std::move(config), .reason = "runtime/tika not found" };
}

bool tika_runtime_process::service_available(
  const std::string& base_url,
  int timeout_ms,
  std::shared_ptr<::wuwe::http_client> http) {
  if (!http) {
    http = std::make_shared<::wuwe::default_http_client>();
  }
  const auto response = http->send({
    .method = "PUT",
    .url = trim_url(base_url) + "/tika",
    .headers = {
      { "Accept", "text/plain" },
      { "Content-Type", "text/plain" },
    },
    .body = "wuwe tika health check",
    .timeout = timeout_ms,
  });
  return !response.error_code;
}

std::shared_ptr<tika_runtime_process> tika_runtime_process::ensure_running(
  tika_runtime_config config) {
  auto discovery = discover(std::move(config));
  if (!discovery.found) {
    return {};
  }

  auto runtime = std::make_shared<tika_runtime_process>();
  runtime->start(std::move(discovery.config));
  return runtime;
}

void tika_runtime_process::start(tika_runtime_config config) {
  config.base_url = default_base_url(config);
  if (service_available(config.base_url, 1000)) {
    impl_->config = std::move(config);
    impl_->own_process = false;
    return;
  }
  if (config.jar_path.empty()) {
    auto discovery = discover(std::move(config));
    if (!discovery.found) {
      throw std::runtime_error("Tika runtime not found: " + discovery.reason);
    }
    config = std::move(discovery.config);
  }
  if (config.java_path.empty()) {
    config.java_path = default_java_path(config.runtime_dir);
  }
  impl_->config = std::move(config);
  impl_->start_process();
  try {
    wait_until_available(impl_->config);
  }
  catch (...) {
    impl_->stop();
    throw;
  }
}

void tika_runtime_process::stop() {
  impl_->stop();
}

bool tika_runtime_process::running() const {
  return impl_->running();
}

bool tika_runtime_process::owns_process() const noexcept {
  return impl_->own_process;
}

const std::string& tika_runtime_process::base_url() const noexcept {
  return impl_->config.base_url;
}

const tika_runtime_config& tika_runtime_process::config() const noexcept {
  return impl_->config;
}

} // namespace wuwe::agent::knowledge
