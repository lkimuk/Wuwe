#include <wuwe/agent/mcp/mcp_process_client.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <cstring>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace wuwe::agent::mcp {
namespace {

std::string header_for(std::string_view message) {
  return "Content-Length: " + std::to_string(message.size()) + "\r\n\r\n";
}

std::string trim(std::string value) {
  const auto begin = value.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) {
    return {};
  }
  const auto end = value.find_last_not_of(" \t\r\n");
  return value.substr(begin, end - begin + 1);
}

bool equals_ascii_case(std::string_view left, std::string_view right) {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t i = 0; i < left.size(); ++i) {
    auto a = static_cast<unsigned char>(left[i]);
    auto b = static_cast<unsigned char>(right[i]);
    if (std::tolower(a) != std::tolower(b)) {
      return false;
    }
  }
  return true;
}

std::size_t parse_content_length(std::string_view value) {
  if (value.empty()) {
    throw std::runtime_error("MCP process response missing Content-Length value");
  }

  std::size_t result = 0;
  for (const auto ch : value) {
    if (ch < '0' || ch > '9') {
      throw std::runtime_error("MCP process response has invalid Content-Length");
    }
    result = result * 10 + static_cast<std::size_t>(ch - '0');
  }
  return result;
}

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

std::string build_command_line(const mcp_process_command& command) {
  if (command.command.empty()) {
    throw std::runtime_error("MCP process command must not be empty");
  }

  std::string output = quote_windows_arg(command.command);
  for (const auto& arg : command.args) {
    output += " ";
    output += quote_windows_arg(arg);
  }
  return output;
}

#ifdef _WIN32
std::string build_windows_environment_block(
  const std::map<std::string, std::string>& overrides) {
  if (overrides.empty()) {
    return {};
  }

  std::map<std::string, std::string> variables;
  LPCH current = GetEnvironmentStringsA();
  if (!current) {
    throw std::runtime_error("failed to read current process environment");
  }
  for (LPCH entry = current; *entry != '\0'; entry += std::strlen(entry) + 1) {
    if (*entry == '=') {
      continue;
    }
    const auto text = std::string(entry);
    const auto separator = text.find('=');
    if (separator == std::string::npos || separator == 0) {
      continue;
    }
    variables[text.substr(0, separator)] = text.substr(separator + 1);
  }
  FreeEnvironmentStringsA(current);

  for (const auto& [key, value] : overrides) {
    if (!key.empty()) {
      variables[key] = value;
    }
  }

  std::string block;
  for (const auto& [key, value] : variables) {
    block += key;
    block += '=';
    block += value;
    block.push_back('\0');
  }
  block.push_back('\0');
  return block;
}
#endif

} // namespace

struct mcp_process_client::impl {
#ifdef _WIN32
  HANDLE process { nullptr };
  HANDLE thread { nullptr };
  HANDLE child_stdin { nullptr };
  HANDLE child_stdout { nullptr };
  HANDLE child_stderr { nullptr };
  std::thread stderr_reader;
  std::mutex io_mutex;
  mutable std::mutex stderr_mutex;
  std::string stderr_buffer;

  ~impl() {
    stop();
  }

  void start(const mcp_process_command& command) {
    if (running()) {
      throw std::runtime_error("MCP process client is already running");
    }

    SECURITY_ATTRIBUTES security {
      .nLength = sizeof(SECURITY_ATTRIBUTES),
      .lpSecurityDescriptor = nullptr,
      .bInheritHandle = TRUE,
    };

    HANDLE stdout_read = nullptr;
    HANDLE stdout_write = nullptr;
    HANDLE stdin_read = nullptr;
    HANDLE stdin_write = nullptr;
    HANDLE stderr_read = nullptr;
    HANDLE stderr_write = nullptr;
    if (!CreatePipe(&stdout_read, &stdout_write, &security, 0)) {
      throw std::runtime_error("failed to create MCP process stdout pipe");
    }
    if (!SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0)) {
      CloseHandle(stdout_read);
      CloseHandle(stdout_write);
      throw std::runtime_error("failed to configure MCP process stdout pipe");
    }
    if (!CreatePipe(&stdin_read, &stdin_write, &security, 0)) {
      CloseHandle(stdout_read);
      CloseHandle(stdout_write);
      throw std::runtime_error("failed to create MCP process stdin pipe");
    }
    if (!SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0)) {
      CloseHandle(stdout_read);
      CloseHandle(stdout_write);
      CloseHandle(stdin_read);
      CloseHandle(stdin_write);
      throw std::runtime_error("failed to configure MCP process stdin pipe");
    }
    if (!CreatePipe(&stderr_read, &stderr_write, &security, 0)) {
      CloseHandle(stdout_read);
      CloseHandle(stdout_write);
      CloseHandle(stdin_read);
      CloseHandle(stdin_write);
      throw std::runtime_error("failed to create MCP process stderr pipe");
    }
    if (!SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0)) {
      CloseHandle(stdout_read);
      CloseHandle(stdout_write);
      CloseHandle(stdin_read);
      CloseHandle(stdin_write);
      CloseHandle(stderr_read);
      CloseHandle(stderr_write);
      throw std::runtime_error("failed to configure MCP process stderr pipe");
    }

    STARTUPINFOA startup {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = stdin_read;
    startup.hStdOutput = stdout_write;
    startup.hStdError = stderr_write;

    PROCESS_INFORMATION info {};
    auto command_line = build_command_line(command);
    auto environment_block = build_windows_environment_block(command.environment);
    const char* working_directory =
      command.working_directory.empty() ? nullptr : command.working_directory.c_str();
    const BOOL created = CreateProcessA(
      nullptr,
      command_line.data(),
      nullptr,
      nullptr,
      TRUE,
      0,
      environment_block.empty() ? nullptr : environment_block.data(),
      working_directory,
      &startup,
      &info);

    CloseHandle(stdin_read);
    CloseHandle(stdout_write);
    CloseHandle(stderr_write);

    if (!created) {
      CloseHandle(stdout_read);
      CloseHandle(stdin_write);
      CloseHandle(stderr_read);
      throw std::runtime_error("failed to start MCP process");
    }

    process = info.hProcess;
    thread = info.hThread;
    child_stdin = stdin_write;
    child_stdout = stdout_read;
    child_stderr = stderr_read;
    clear_stderr_output();
    start_stderr_reader();
  }

  void stop() {
    if (child_stdin) {
      CloseHandle(child_stdin);
      child_stdin = nullptr;
    }
    if (process) {
      const auto wait_result = WaitForSingleObject(process, 2000);
      if (wait_result == WAIT_TIMEOUT) {
        TerminateProcess(process, 1);
        WaitForSingleObject(process, 2000);
      }
    }
    join_stderr_reader();
    if (child_stdout) {
      CloseHandle(child_stdout);
      child_stdout = nullptr;
    }
    if (child_stderr) {
      CloseHandle(child_stderr);
      child_stderr = nullptr;
    }
    if (thread) {
      CloseHandle(thread);
      thread = nullptr;
    }
    if (process) {
      CloseHandle(process);
      process = nullptr;
    }
  }

  bool running() {
    if (!process) {
      return false;
    }
    DWORD exit_code = 0;
    return GetExitCodeProcess(process, &exit_code) && exit_code == STILL_ACTIVE;
  }

  void write_all(std::string_view data) {
    if (!child_stdin) {
      throw std::runtime_error("MCP process stdin is closed");
    }
    const char* cursor = data.data();
    std::size_t remaining = data.size();
    while (remaining > 0) {
      DWORD written = 0;
      const auto chunk = static_cast<DWORD>((std::min<std::size_t>)(remaining, 65536));
      if (!WriteFile(child_stdin, cursor, chunk, &written, nullptr) || written == 0) {
        throw std::runtime_error("failed to write MCP process stdin");
      }
      cursor += written;
      remaining -= written;
    }
  }

  char read_char() {
    char ch = '\0';
    DWORD read = 0;
    if (!child_stdout || !ReadFile(child_stdout, &ch, 1, &read, nullptr) || read != 1) {
      throw std::runtime_error("failed to read MCP process stdout");
    }
    return ch;
  }

  std::string read_exact(std::size_t size) {
    std::string output(size, '\0');
    std::size_t offset = 0;
    while (offset < size) {
      DWORD read = 0;
      const auto chunk = static_cast<DWORD>((std::min<std::size_t>)(size - offset, 65536));
      if (!child_stdout ||
          !ReadFile(child_stdout, output.data() + offset, chunk, &read, nullptr) ||
          read == 0) {
        throw std::runtime_error("failed to read MCP process response body");
      }
      offset += read;
    }
    return output;
  }

  void start_stderr_reader() {
    stderr_reader = std::thread([this] {
      char buffer[4096];
      while (child_stderr) {
        DWORD read = 0;
        if (!ReadFile(child_stderr, buffer, sizeof(buffer), &read, nullptr) || read == 0) {
          break;
        }
        std::lock_guard lock(stderr_mutex);
        stderr_buffer.append(buffer, buffer + read);
      }
    });
  }

  void join_stderr_reader() {
    if (stderr_reader.joinable()) {
      stderr_reader.join();
    }
  }

  std::string stderr_output() const {
    std::lock_guard lock(stderr_mutex);
    return stderr_buffer;
  }

  void clear_stderr_output() {
    std::lock_guard lock(stderr_mutex);
    stderr_buffer.clear();
  }
#else
  pid_t pid { -1 };
  int child_stdin { -1 };
  int child_stdout { -1 };
  int child_stderr { -1 };
  std::thread stderr_reader;
  std::mutex io_mutex;
  mutable std::mutex stderr_mutex;
  std::string stderr_buffer;

  ~impl() {
    stop();
  }

  void start(const mcp_process_command& command) {
    if (running()) {
      throw std::runtime_error("MCP process client is already running");
    }
    if (command.command.empty()) {
      throw std::runtime_error("MCP process command must not be empty");
    }

    int stdin_pipe[2] { -1, -1 };
    int stdout_pipe[2] { -1, -1 };
    int stderr_pipe[2] { -1, -1 };
    if (pipe(stdin_pipe) != 0 || pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0) {
      throw std::runtime_error("failed to create MCP process pipes");
    }

    const auto child = fork();
    if (child < 0) {
      close(stdin_pipe[0]);
      close(stdin_pipe[1]);
      close(stdout_pipe[0]);
      close(stdout_pipe[1]);
      close(stderr_pipe[0]);
      close(stderr_pipe[1]);
      throw std::runtime_error("failed to fork MCP process");
    }

    if (child == 0) {
      if (!command.working_directory.empty()) {
        (void)chdir(command.working_directory.c_str());
      }
      for (const auto& [key, value] : command.environment) {
        if (!key.empty()) {
          (void)setenv(key.c_str(), value.c_str(), 1);
        }
      }
      dup2(stdin_pipe[0], STDIN_FILENO);
      dup2(stdout_pipe[1], STDOUT_FILENO);
      dup2(stderr_pipe[1], STDERR_FILENO);
      close(stdin_pipe[0]);
      close(stdin_pipe[1]);
      close(stdout_pipe[0]);
      close(stdout_pipe[1]);
      close(stderr_pipe[0]);
      close(stderr_pipe[1]);

      std::vector<std::string> storage;
      storage.reserve(command.args.size() + 1);
      storage.push_back(command.command);
      for (const auto& arg : command.args) {
        storage.push_back(arg);
      }
      std::vector<char*> argv;
      argv.reserve(storage.size() + 1);
      for (auto& value : storage) {
        argv.push_back(value.data());
      }
      argv.push_back(nullptr);
      execvp(command.command.c_str(), argv.data());
      _exit(127);
    }

    close(stdin_pipe[0]);
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);
    pid = child;
    child_stdin = stdin_pipe[1];
    child_stdout = stdout_pipe[0];
    child_stderr = stderr_pipe[0];
    clear_stderr_output();
    start_stderr_reader();
  }

  void stop() {
    if (child_stdin >= 0) {
      close(child_stdin);
      child_stdin = -1;
    }
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
    join_stderr_reader();
    if (child_stdout >= 0) {
      close(child_stdout);
      child_stdout = -1;
    }
    if (child_stderr >= 0) {
      close(child_stderr);
      child_stderr = -1;
    }
  }

  bool running() {
    if (pid <= 0) {
      return false;
    }
    int status = 0;
    const auto result = waitpid(pid, &status, WNOHANG);
    if (result == pid) {
      pid = -1;
      return false;
    }
    return result == 0;
  }

  void write_all(std::string_view data) {
    if (child_stdin < 0) {
      throw std::runtime_error("MCP process stdin is closed");
    }
    const char* cursor = data.data();
    std::size_t remaining = data.size();
    while (remaining > 0) {
      const auto written = write(child_stdin, cursor, remaining);
      if (written <= 0) {
        throw std::runtime_error("failed to write MCP process stdin");
      }
      cursor += written;
      remaining -= static_cast<std::size_t>(written);
    }
  }

  char read_char() {
    char ch = '\0';
    const auto result = read(child_stdout, &ch, 1);
    if (result != 1) {
      throw std::runtime_error("failed to read MCP process stdout");
    }
    return ch;
  }

  std::string read_exact(std::size_t size) {
    std::string output(size, '\0');
    std::size_t offset = 0;
    while (offset < size) {
      const auto result = read(child_stdout, output.data() + offset, size - offset);
      if (result <= 0) {
        throw std::runtime_error("failed to read MCP process response body");
      }
      offset += static_cast<std::size_t>(result);
    }
    return output;
  }

  void start_stderr_reader() {
    stderr_reader = std::thread([this] {
      char buffer[4096];
      while (child_stderr >= 0) {
        const auto result = read(child_stderr, buffer, sizeof(buffer));
        if (result <= 0) {
          break;
        }
        std::lock_guard lock(stderr_mutex);
        stderr_buffer.append(buffer, buffer + result);
      }
    });
  }

  void join_stderr_reader() {
    if (stderr_reader.joinable()) {
      stderr_reader.join();
    }
  }

  std::string stderr_output() const {
    std::lock_guard lock(stderr_mutex);
    return stderr_buffer;
  }

  void clear_stderr_output() {
    std::lock_guard lock(stderr_mutex);
    stderr_buffer.clear();
  }
#endif

  void write_framed_message(std::string_view message) {
    const auto header = header_for(message);
    write_all(header);
    write_all(message);
  }

  std::string read_framed_message() {
    std::string header;
    while (header.find("\r\n\r\n") == std::string::npos &&
           header.find("\n\n") == std::string::npos) {
      header.push_back(read_char());
    }

    std::size_t content_length = 0;
    std::size_t cursor = 0;
    while (cursor < header.size()) {
      const auto next = header.find('\n', cursor);
      auto line = header.substr(cursor, next == std::string::npos ? std::string::npos : next - cursor);
      cursor = next == std::string::npos ? header.size() : next + 1;
      line = trim(std::move(line));
      if (line.empty()) {
        continue;
      }
      const auto separator = line.find(':');
      if (separator == std::string::npos) {
        continue;
      }
      if (equals_ascii_case(trim(line.substr(0, separator)), "Content-Length")) {
        content_length = parse_content_length(trim(line.substr(separator + 1)));
      }
    }

    if (content_length == 0) {
      throw std::runtime_error("MCP process response missing Content-Length");
    }
    return read_exact(content_length);
  }
};

mcp_process_client::mcp_process_client()
    : impl_(std::make_unique<impl>()) {
}

mcp_process_client::mcp_process_client(mcp_process_command command)
    : mcp_process_client() {
  start(std::move(command));
}

mcp_process_client::~mcp_process_client() = default;

mcp_process_client::mcp_process_client(mcp_process_client&&) noexcept = default;

mcp_process_client& mcp_process_client::operator=(mcp_process_client&&) noexcept = default;

void mcp_process_client::start(mcp_process_command command) {
  std::lock_guard lock(impl_->io_mutex);
  impl_->start(command);
}

void mcp_process_client::stop() {
  std::lock_guard lock(impl_->io_mutex);
  impl_->stop();
}

bool mcp_process_client::running() {
  std::lock_guard lock(impl_->io_mutex);
  return impl_->running();
}

json mcp_process_client::request(std::string method, json params) {
  std::lock_guard lock(impl_->io_mutex);
  if (!impl_->running()) {
    throw std::runtime_error("MCP process client is not running");
  }

  const auto id = next_id_++;
  impl_->write_framed_message(make_request(id, std::move(method), std::move(params)));
  return read_response(id);
}

void mcp_process_client::notify(std::string method, json params) {
  std::lock_guard lock(impl_->io_mutex);
  if (!impl_->running()) {
    throw std::runtime_error("MCP process client is not running");
  }
  impl_->write_framed_message(make_notification(std::move(method), std::move(params)));
}

json mcp_process_client::initialize(mcp_client_info info, json capabilities) {
  json client_info = json::object();
  if (!info.name.empty()) {
    client_info["name"] = std::move(info.name);
  }
  if (!info.version.empty()) {
    client_info["version"] = std::move(info.version);
  }

  return request("initialize", {
    { "protocolVersion", default_protocol_version },
    { "clientInfo", std::move(client_info) },
    { "capabilities", std::move(capabilities) },
  });
}

json mcp_process_client::ping() {
  return request("ping");
}

json mcp_process_client::list_tools(json params) {
  return request("tools/list", std::move(params));
}

json mcp_process_client::call_tool(std::string name, json arguments) {
  return request("tools/call", {
    { "name", std::move(name) },
    { "arguments", std::move(arguments) },
  });
}

json mcp_process_client::list_resources(json params) {
  return request("resources/list", std::move(params));
}

json mcp_process_client::read_resource(std::string uri) {
  return request("resources/read", { { "uri", std::move(uri) } });
}

json mcp_process_client::subscribe_resource(std::string uri) {
  return request("resources/subscribe", { { "uri", std::move(uri) } });
}

json mcp_process_client::unsubscribe_resource(std::string uri) {
  return request("resources/unsubscribe", { { "uri", std::move(uri) } });
}

json mcp_process_client::list_resource_templates(json params) {
  return request("resources/templates/list", std::move(params));
}

json mcp_process_client::list_roots(json params) {
  return request("roots/list", std::move(params));
}

json mcp_process_client::list_prompts(json params) {
  return request("prompts/list", std::move(params));
}

json mcp_process_client::get_prompt(std::string name, json arguments) {
  return request("prompts/get", {
    { "name", std::move(name) },
    { "arguments", std::move(arguments) },
  });
}

const std::vector<json>& mcp_process_client::notifications() const noexcept {
  return notifications_;
}

void mcp_process_client::clear_notifications() {
  notifications_.clear();
}

std::string mcp_process_client::stderr_output() const {
  return impl_->stderr_output();
}

void mcp_process_client::clear_stderr_output() {
  impl_->clear_stderr_output();
}

json mcp_process_client::read_response(int id) {
  while (true) {
    auto parsed = json::parse(impl_->read_framed_message());
    if (parsed.is_object() && !parsed.contains("id") && parsed.contains("method")) {
      notifications_.push_back(std::move(parsed));
      continue;
    }
    if (!parsed.is_object() || !parsed.contains("id")) {
      throw std::runtime_error("MCP process client received invalid response");
    }
    if (parsed["id"] != id) {
      notifications_.push_back(std::move(parsed));
      continue;
    }
    return parsed;
  }
}

} // namespace wuwe::agent::mcp
