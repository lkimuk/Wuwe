#include <csignal>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include <wuwe/agent/host/host_codec.hpp>
#include <wuwe/runtime_host.hpp>
#include <wuwe/version.hpp>

namespace {

enum class exit_code : int {
  success = 0,
  usage = 2,
  config = 3,
  startup = 4,
  execution = 5,
};

volatile std::sig_atomic_t interrupted = 0;

extern "C" void handle_signal(int) {
  interrupted = 1;
}

struct command_line {
  std::string command;
  std::optional<std::string> config_path;
  std::optional<std::string> package_path;
  std::optional<std::string> prompt;
};

void print_usage(std::ostream& output) {
  output << "Usage:\n"
            "  wuwe-runtime version\n"
            "  wuwe-runtime validate --config <runtime.json>\n"
            "  wuwe-runtime doctor --config <runtime.json>\n"
            "  wuwe-runtime inspect-plugin --package <directory>\n"
            "  wuwe-runtime run --config <runtime.json> [--prompt <text>]\n"
            "  wuwe-runtime serve --config <runtime.json>\n";
}

command_line parse_arguments(int argc, char** argv) {
  if (argc < 2)
    throw std::invalid_argument("a command is required");
  command_line result { .command = argv[1] };
  if (result.command == "--help" || result.command == "-h")
    result.command = "help";
  for (int index = 2; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--config" || argument == "--package" || argument == "--prompt") {
      if (index + 1 >= argc)
        throw std::invalid_argument(std::string(argument) + " requires a value");
      const std::string value(argv[++index]);
      if (argument == "--config") {
        if (result.config_path)
          throw std::invalid_argument("--config may only be specified once");
        result.config_path = value;
      }
      else if (argument == "--package") {
        if (result.package_path)
          throw std::invalid_argument("--package may only be specified once");
        result.package_path = value;
      }
      else {
        if (result.prompt)
          throw std::invalid_argument("--prompt may only be specified once");
        result.prompt = value;
      }
      continue;
    }
    throw std::invalid_argument("unknown argument: " + std::string(argument));
  }
  if (result.command != "version" && result.command != "help" &&
      result.command != "inspect-plugin" && !result.config_path)
    throw std::invalid_argument("--config is required for command '" + result.command + "'");
  if (result.command == "inspect-plugin" && !result.package_path)
    throw std::invalid_argument("--package is required for command 'inspect-plugin'");
  if (result.command != "inspect-plugin" && result.package_path)
    throw std::invalid_argument("--package is only valid for the inspect-plugin command");
  if (result.command == "inspect-plugin" && result.config_path)
    throw std::invalid_argument("inspect-plugin does not accept --config");
  if (result.command != "run" && result.prompt)
    throw std::invalid_argument("--prompt is only valid for the run command");
  return result;
}

nlohmann::json issues_to_json(const wuwe::runtime_host::validation_report& report) {
  auto issues = nlohmann::json::array();
  for (const auto& issue : report.issues) {
    issues.push_back(
      { { "path", issue.path }, { "code", issue.code }, { "message", issue.message } });
  }
  return issues;
}

std::string read_prompt(const std::optional<std::string>& argument) {
  if (argument)
    return *argument;
  std::ostringstream input;
  input << std::cin.rdbuf();
  auto prompt = input.str();
  while (!prompt.empty() && (prompt.back() == '\n' || prompt.back() == '\r'))
    prompt.pop_back();
  if (prompt.empty())
    throw std::invalid_argument("run requires --prompt or a prompt on standard input");
  return prompt;
}

nlohmann::json malformed_request_response(std::string message) {
  return wuwe::agent::host::host_response_to_json({
    .protocol_version = std::string(wuwe::agent::host::default_protocol_version),
    .operation = wuwe::agent::host::host_operation::get_run,
    .error =
      wuwe::agent::host::host_error {
        .code = wuwe::agent::host::host_error_code::invalid_request,
        .message = std::move(message),
      },
  });
}

int serve(wuwe::runtime_host::host& host) {
  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);
  std::string line;
  while (interrupted == 0 && std::getline(std::cin, line)) {
    if (line.empty())
      continue;
    try {
      std::cout << host.dispatch_protocol_request(nlohmann::json::parse(line)).dump() << '\n'
                << std::flush;
    }
    catch (const nlohmann::json::exception&) {
      std::cout << malformed_request_response("malformed JSON request").dump() << '\n'
                << std::flush;
    }
    catch (...) {
      std::cout << malformed_request_response("runtime request could not be processed").dump()
                << '\n'
                << std::flush;
    }
  }
  return host.stop() ? static_cast<int>(exit_code::success) : static_cast<int>(exit_code::startup);
}

} // namespace

int main(int argc, char** argv) {
  try {
    const auto command = parse_arguments(argc, argv);
    if (command.command == "help") {
      if (command.config_path || command.package_path || command.prompt)
        throw std::invalid_argument("help does not accept options");
      print_usage(std::cout);
      return static_cast<int>(exit_code::success);
    }
    if (command.command == "version") {
      if (command.config_path || command.package_path || command.prompt)
        throw std::invalid_argument("version does not accept options");
      std::cout << "wuwe-runtime " << wuwe::framework_version << '\n';
      return static_cast<int>(exit_code::success);
    }
    if (command.command != "validate" && command.command != "doctor" &&
        command.command != "inspect-plugin" && command.command != "run" &&
        command.command != "serve") {
      throw std::invalid_argument("unknown command: " + command.command);
    }

    if (command.command == "inspect-plugin") {
      const auto package = wuwe::runtime_host::load_plugin_package(*command.package_path);
      std::cout << nlohmann::json {
        { "valid", true },
        { "contentSha256", package->content_sha256() },
        { "manifest", wuwe::runtime_host::plugin_manifest_to_json(package->manifest()) },
      }.dump(2) << '\n';
      return static_cast<int>(exit_code::success);
    }

    const auto config = wuwe::runtime_host::load_runtime_config(*command.config_path);
    if (command.command == "validate") {
      std::cout << nlohmann::json {
        { "valid", true }, { "schemaVersion", config.schema_version }
      }.dump(2) << '\n';
      return static_cast<int>(exit_code::success);
    }
    if (command.command == "doctor") {
      auto report = wuwe::runtime_host::diagnose_runtime_config(config);
      if (report.ready) {
        try {
          wuwe::runtime_host::host probe;
          probe.configure(config);
          probe.start();
          report.checks["assembly"] = {
            .ok = probe.health().status == wuwe::runtime_host::health_status::healthy,
            .message = "Runtime Host dependencies assembled successfully",
          };
          if (!probe.stop()) {
            report.checks["assembly"] = {
              .ok = false,
              .message = "Runtime Host dependencies assembled but did not stop cleanly",
            };
          }
        }
        catch (const std::exception& ex) {
          report.checks["assembly"] = {
            .ok = false,
            .message = ex.what(),
          };
        }
        report.ready = report.checks.at("assembly").ok;
      }
      std::cout << wuwe::runtime_host::doctor_report_to_json(report).dump(2) << '\n';
      return report.ready ? static_cast<int>(exit_code::success)
                          : static_cast<int>(exit_code::startup);
    }

    wuwe::runtime_host::host host;
    host.configure(config);
    host.start();
    if (command.command == "serve")
      return serve(host);

    const auto result = host.run(read_prompt(command.prompt));
    std::cout << wuwe::runtime_host::run_result_to_json(result).dump(2) << '\n';
    const auto succeeded = result.status == wuwe::agent::runtime::agent_run_status::completed;
    if (!host.stop())
      return static_cast<int>(exit_code::startup);
    return succeeded ? static_cast<int>(exit_code::success)
                     : static_cast<int>(exit_code::execution);
  }
  catch (const wuwe::runtime_host::validation_error& ex) {
    std::cerr << nlohmann::json {
      { "error", "invalid_config" },
      { "message", ex.what() },
      { "issues", issues_to_json(ex.report()) }
    }.dump(2) << '\n';
    return static_cast<int>(exit_code::config);
  }
  catch (const wuwe::runtime_host::plugin_package_error& ex) {
    std::cerr << nlohmann::json {
      { "error", wuwe::runtime_host::to_string(ex.code()) }, { "message", ex.what() }
    }.dump(2) << '\n';
    return static_cast<int>(exit_code::config);
  }
  catch (const std::invalid_argument& ex) {
    std::cerr << "wuwe-runtime: " << ex.what() << '\n';
    print_usage(std::cerr);
    return static_cast<int>(exit_code::usage);
  }
  catch (const wuwe::runtime_host::runtime_error& ex) {
    std::cerr << nlohmann::json {
      { "error", wuwe::runtime_host::to_string(ex.code()) }, { "message", ex.what() }
    }.dump(2) << '\n';
    return ex.code() == wuwe::runtime_host::runtime_error_code::execution_failed
             ? static_cast<int>(exit_code::execution)
             : static_cast<int>(exit_code::startup);
  }
  catch (const std::exception& ex) {
    std::cerr << nlohmann::json { { "error", "startup_failed" }, { "message", ex.what() } }.dump(2)
              << '\n';
    return static_cast<int>(exit_code::startup);
  }
}
