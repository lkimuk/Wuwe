#include <wuwe/agent/knowledge/tika_runtime.hpp>

#include <chrono>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <wuwe/net/default_http_client.h>

#include "tika_runtime_port.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <csignal>
#include <mach-o/dyld.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#else
#include <csignal>
#include <limits.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace wuwe::agent::knowledge {
namespace {

constexpr int automatic_port_attempts = 3;

class runtime_process_exit_error : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

std::string trim_url(std::string value) {
  while (!value.empty() && value.back() == '/') {
    value.pop_back();
  }
  return value;
}

void validate_config(const tika_runtime_config& config) {
  if (config.port < 0 || config.port > 65535) {
    throw std::invalid_argument("Tika runtime port must be between 0 and 65535");
  }
  if (config.startup_timeout_ms <= 0) {
    throw std::invalid_argument("Tika runtime startup_timeout_ms must be positive");
  }
  if (config.poll_interval_ms <= 0) {
    throw std::invalid_argument("Tika runtime poll_interval_ms must be positive");
  }
  if (config.base_url.empty() && config.host.empty()) {
    throw std::invalid_argument("Tika runtime host must not be empty");
  }
  if (config.port == 0 && !config.base_url.empty()) {
    throw std::invalid_argument("Tika runtime automatic port cannot be combined with base_url");
  }
}

std::string url_host(const std::string& host) {
  if (host.find(':') != std::string::npos && (host.empty() || host.front() != '[')) {
    return "[" + host + "]";
  }
  return host;
}

std::string default_base_url(const tika_runtime_config& config) {
  if (!config.base_url.empty()) {
    return trim_url(config.base_url);
  }
  if (config.port == 0) {
    return {};
  }
  return "http://" + url_host(config.host) + ":" + std::to_string(config.port);
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
#elif defined(__APPLE__)
  std::uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::string buffer(size, '\0');
  if (_NSGetExecutablePath(buffer.data(), &size) != 0)
    return std::nullopt;
  std::error_code error;
  auto executable = std::filesystem::canonical(buffer.c_str(), error);
  return error ? std::nullopt : std::optional(executable.parent_path());
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
#elif defined(__APPLE__)
  const auto bundled = runtime_dir.parent_path() / "jre" / "Contents" / "Home" / "bin" / "java";
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
  output += " --noFork";
  return output;
}
#endif

} // namespace

struct tika_runtime_process::impl {
  tika_runtime_config config;
  bool own_process { false };

#ifdef _WIN32
  HANDLE process { nullptr };
  HANDLE thread { nullptr };
  HANDLE job { nullptr };
#else
  mutable pid_t pid { -1 };
  mutable int exit_status { 0 };
  mutable bool has_exit_status { false };
#endif

  ~impl() {
    if (config.stop_on_destroy) {
      stop_process();
    } else {
      release_process();
    }
  }

  void start_process() {
#ifdef _WIN32
    STARTUPINFOA startup {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info {};
    auto command_line = build_windows_command_line(config);
    const auto working_directory =
      config.runtime_dir.empty() ? std::string {} : config.runtime_dir.string();
    const auto created = CreateProcessA(nullptr,
      command_line.data(),
      nullptr,
      nullptr,
      FALSE,
      CREATE_NO_WINDOW | CREATE_SUSPENDED,
      nullptr,
      working_directory.empty() ? nullptr : working_directory.c_str(),
      &startup,
      &info);
    if (!created) {
      throw std::runtime_error("failed to start bundled Tika runtime");
    }
    process = info.hProcess;
    thread = info.hThread;
    if (config.stop_on_destroy) {
      job = CreateJobObjectA(nullptr, nullptr);
      JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
      limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
      if (!job
        || !SetInformationJobObject(
          job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))
        || !AssignProcessToJobObject(job, process)) {
        const DWORD error = GetLastError();
        (void)TerminateProcess(process, 1);
        (void)WaitForSingleObject(process, 2000);
        CloseHandle(thread);
        CloseHandle(process);
        if (job) {
          CloseHandle(job);
        }
        thread = nullptr;
        process = nullptr;
        job = nullptr;
        throw std::system_error(
          static_cast<int>(error), std::system_category(), "failed to manage Tika process job");
      }
    }
    if (ResumeThread(thread) == static_cast<DWORD>(-1)) {
      const DWORD error = GetLastError();
      (void)TerminateProcess(process, 1);
      (void)WaitForSingleObject(process, 2000);
      CloseHandle(thread);
      CloseHandle(process);
      if (job) {
        CloseHandle(job);
      }
      thread = nullptr;
      process = nullptr;
      job = nullptr;
      throw std::system_error(
        static_cast<int>(error), std::system_category(), "failed to manage Tika process job");
    }
    own_process = true;
#else
    const auto child = fork();
    if (child < 0) {
      throw std::runtime_error("failed to fork bundled Tika runtime");
    }
    if (child == 0) {
      if (!config.runtime_dir.empty() && chdir(config.runtime_dir.string().c_str()) != 0) {
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
        "--noFork",
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
    has_exit_status = false;
#endif
  }

  void stop_process() {
    if (!own_process) {
      return;
    }
#ifdef _WIN32
    if (job) {
      (void)TerminateJobObject(job, 1);
    } else if (process) {
      (void)TerminateProcess(process, 1);
    }
    if (process) {
      (void)WaitForSingleObject(process, 2000);
    }
    if (thread) {
      CloseHandle(thread);
      thread = nullptr;
    }
    if (process) {
      CloseHandle(process);
      process = nullptr;
    }
    if (job) {
      CloseHandle(job);
      job = nullptr;
    }
#else
    if (pid > 0) {
      (void)kill(pid, SIGTERM);
      for (int i = 0; i < 20; ++i) {
        int status = 0;
        const auto result = waitpid(pid, &status, WNOHANG);
        if (result == pid) {
          exit_status = status;
          has_exit_status = true;
          pid = -1;
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      if (pid > 0) {
        (void)kill(pid, SIGKILL);
        (void)waitpid(pid, nullptr, 0);
        pid = -1;
      }
    }
#endif
    own_process = false;
  }

  void release_process() {
    if (!own_process) {
      return;
    }
#ifdef _WIN32
    if (job) {
      CloseHandle(job);
      job = nullptr;
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
      const pid_t child = pid;
      std::thread([child] {
        (void)waitpid(child, nullptr, 0);
      }).detach();
      pid = -1;
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
    if (!GetExitCodeProcess(process, &exit_code)) {
      return false;
    }
    return exit_code == STILL_ACTIVE;
#else
    if (pid <= 0) {
      return false;
    }
    int status = 0;
    const auto result = waitpid(pid, &status, WNOHANG);
    if (result == 0) {
      return true;
    }
    if (result == pid) {
      pid = -1;
      exit_status = status;
      has_exit_status = true;
    }
    return false;
#endif
  }

  std::string exit_description() const {
#ifdef _WIN32
    DWORD exit_code = 0;
    if (process && GetExitCodeProcess(process, &exit_code) && exit_code != STILL_ACTIVE) {
      return " (exit code " + std::to_string(exit_code) + ")";
    }
#else
    if (has_exit_status) {
      if (WIFEXITED(exit_status)) {
        return " (exit code " + std::to_string(WEXITSTATUS(exit_status)) + ")";
      }
      if (WIFSIGNALED(exit_status)) {
        return " (signal " + std::to_string(WTERMSIG(exit_status)) + ")";
      }
    }
#endif
    return {};
  }

  void wait_until_available() {
    const auto started = std::chrono::steady_clock::now();
    const auto timeout = std::chrono::milliseconds(config.startup_timeout_ms);
    const auto interval = std::chrono::milliseconds(config.poll_interval_ms);
    while (std::chrono::steady_clock::now() - started < timeout) {
      if (!running()) {
        throw runtime_process_exit_error("Tika runtime exited before becoming available at "
          + config.base_url + exit_description());
      }
      if (tika_runtime_process::service_available(config.base_url, 1000)) {
        if (!running()) {
          throw runtime_process_exit_error("Tika runtime exited during startup at " + config.base_url
            + exit_description());
        }
        return;
      }
      std::this_thread::sleep_for(interval);
    }
    throw std::runtime_error("Tika runtime did not become available at " + config.base_url);
  }
};

tika_runtime_process::tika_runtime_process() : impl_(std::make_unique<impl>()) {
}

tika_runtime_process::~tika_runtime_process() = default;

tika_runtime_process::tika_runtime_process(tika_runtime_process&&) noexcept = default;

tika_runtime_process& tika_runtime_process::operator=(tika_runtime_process&&) noexcept = default;

tika_runtime_discovery tika_runtime_process::discover(tika_runtime_config config) {
  validate_config(config);
  if (config.port != 0) {
    config.base_url = default_base_url(config);
  }
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
  const std::string& base_url, int timeout_ms, std::shared_ptr<::wuwe::http_client> http) {
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
  validate_config(config);
  auto discovery = discover(std::move(config));
  if (!discovery.found) {
    return {};
  }

  auto runtime = std::make_shared<tika_runtime_process>();
  runtime->start(std::move(discovery.config));
  return runtime;
}

void tika_runtime_process::start(tika_runtime_config config) {
  validate_config(config);
  impl_->stop_process();

  const bool automatic_port = config.port == 0;
  if (!automatic_port) {
    config.base_url = default_base_url(config);
    if (service_available(config.base_url, 1000)) {
      impl_->config = std::move(config);
      impl_->own_process = false;
      return;
    }
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
  if (!automatic_port) {
    impl_->config = std::move(config);
    impl_->start_process();
    try {
      impl_->wait_until_available();
    }
    catch (...) {
      impl_->stop_process();
      throw;
    }
    return;
  }

  std::string last_error;
  for (int attempt = 0; attempt < automatic_port_attempts; ++attempt) {
    auto candidate = config;
    candidate.port = detail::select_available_loopback_port(candidate.host);
    candidate.base_url = default_base_url(candidate);
    impl_->config = std::move(candidate);
    try {
      impl_->start_process();
      impl_->wait_until_available();
      return;
    }
    catch (const runtime_process_exit_error& error) {
      last_error = error.what();
      impl_->stop_process();
    }
    catch (...) {
      impl_->stop_process();
      throw;
    }
  }
  throw std::runtime_error("Tika runtime failed to start on an automatically selected port after "
    + std::to_string(automatic_port_attempts) + " attempts: " + last_error);
}

void tika_runtime_process::stop() {
  impl_->stop_process();
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
