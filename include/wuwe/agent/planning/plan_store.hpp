#ifndef WUWE_AGENT_PLANNING_PLAN_STORE_HPP
#define WUWE_AGENT_PLANNING_PLAN_STORE_HPP

#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <wuwe/agent/planning/plan.hpp>

namespace wuwe::agent::planning {

class plan_store {
public:
  virtual ~plan_store() = default;

  virtual void save(const plan& value) = 0;
  virtual std::optional<plan> load(const std::string& id) const = 0;
  virtual std::vector<plan> list() const = 0;
  virtual bool erase(const std::string& id) = 0;
};

class in_memory_plan_store final : public plan_store {
public:
  void save(const plan& value) override {
    plans_[value.id] = value;
  }

  std::optional<plan> load(const std::string& id) const override {
    const auto found = plans_.find(id);
    if (found == plans_.end()) {
      return std::nullopt;
    }
    return found->second;
  }

  std::vector<plan> list() const override {
    std::vector<plan> output;
    output.reserve(plans_.size());
    for (const auto& [_, value] : plans_) {
      output.push_back(value);
    }
    return output;
  }

  bool erase(const std::string& id) override {
    return plans_.erase(id) != 0;
  }

private:
  std::map<std::string, plan> plans_;
};

class file_plan_store final : public plan_store {
public:
  explicit file_plan_store(std::filesystem::path path) : path_(std::move(path)) {
  }

  void save(const plan& value) override {
    auto all = read_all();
    all[value.id] = value;
    write_all(all);
  }

  std::optional<plan> load(const std::string& id) const override {
    const auto all = read_all();
    const auto found = all.find(id);
    if (found == all.end()) {
      return std::nullopt;
    }
    return found->second;
  }

  std::vector<plan> list() const override {
    const auto all = read_all();
    std::vector<plan> output;
    output.reserve(all.size());
    for (const auto& [_, value] : all) {
      output.push_back(value);
    }
    return output;
  }

  bool erase(const std::string& id) override {
    auto all = read_all();
    if (all.erase(id) == 0) {
      return false;
    }
    write_all(all);
    return true;
  }

private:
  std::map<std::string, plan> read_all() const {
    std::map<std::string, plan> output;
    if (!std::filesystem::exists(path_)) {
      return output;
    }

    std::ifstream input(path_);
    if (!input) {
      throw std::runtime_error("failed to open plan store: " + path_.string());
    }

    nlohmann::json json;
    input >> json;
    if (!json.is_object()) {
      throw std::runtime_error("plan store root must be a JSON object");
    }
    for (const auto& [id, item] : json.items()) {
      output[id] = plan_from_json(item);
    }
    return output;
  }

  void write_all(const std::map<std::string, plan>& values) const {
    if (path_.has_parent_path()) {
      std::filesystem::create_directories(path_.parent_path());
    }

    nlohmann::json json = nlohmann::json::object();
    for (const auto& [id, value] : values) {
      json[id] = plan_to_json(value);
    }

    std::ofstream output(path_, std::ios::trunc);
    if (!output) {
      throw std::runtime_error("failed to write plan store: " + path_.string());
    }
    output << json.dump(2);
  }

  std::filesystem::path path_;
};

} // namespace wuwe::agent::planning

#endif // WUWE_AGENT_PLANNING_PLAN_STORE_HPP
