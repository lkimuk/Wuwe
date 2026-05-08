#ifndef WUWE_AGENT_REFLECTION_STORE_HPP
#define WUWE_AGENT_REFLECTION_STORE_HPP

#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <wuwe/agent/reflection/reflection_core.hpp>

namespace wuwe::agent::reflection {

class reflection_store {
public:
  virtual ~reflection_store() = default;

  virtual void save(const reflection_record& record) = 0;
  virtual std::optional<reflection_record> load(const std::string& id) const = 0;
  virtual std::vector<reflection_record> list() const = 0;
  virtual bool erase(const std::string& id) = 0;
};

class in_memory_reflection_store final : public reflection_store {
public:
  void save(const reflection_record& record) override {
    records_[record.id] = record;
  }

  std::optional<reflection_record> load(const std::string& id) const override {
    const auto found = records_.find(id);
    if (found == records_.end()) {
      return std::nullopt;
    }
    return found->second;
  }

  std::vector<reflection_record> list() const override {
    std::vector<reflection_record> output;
    output.reserve(records_.size());
    for (const auto& [_, value] : records_) {
      output.push_back(value);
    }
    return output;
  }

  bool erase(const std::string& id) override {
    return records_.erase(id) != 0;
  }

private:
  std::map<std::string, reflection_record> records_;
};

class file_reflection_store final : public reflection_store {
public:
  explicit file_reflection_store(std::filesystem::path path) : path_(std::move(path)) {
  }

  void save(const reflection_record& record) override {
    auto all = read_all();
    all[record.id] = record;
    write_all(all);
  }

  std::optional<reflection_record> load(const std::string& id) const override {
    const auto all = read_all();
    const auto found = all.find(id);
    if (found == all.end()) {
      return std::nullopt;
    }
    return found->second;
  }

  std::vector<reflection_record> list() const override {
    const auto all = read_all();
    std::vector<reflection_record> output;
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
  std::map<std::string, reflection_record> read_all() const {
    std::map<std::string, reflection_record> output;
    if (!std::filesystem::exists(path_)) {
      return output;
    }
    std::ifstream input(path_);
    if (!input) {
      throw std::runtime_error("failed to open reflection store: " + path_.string());
    }

    nlohmann::json json;
    input >> json;
    if (!json.is_object()) {
      throw std::runtime_error("reflection store root must be a JSON object");
    }
    for (const auto& [id, item] : json.items()) {
      output[id] = reflection_codec::record_from_json(item);
    }
    return output;
  }

  void write_all(const std::map<std::string, reflection_record>& values) const {
    if (path_.has_parent_path()) {
      std::filesystem::create_directories(path_.parent_path());
    }
    nlohmann::json json = nlohmann::json::object();
    for (const auto& [id, value] : values) {
      json[id] = reflection_codec::record_to_json(value);
    }

    std::ofstream output(path_, std::ios::trunc);
    if (!output) {
      throw std::runtime_error("failed to write reflection store: " + path_.string());
    }
    output << json.dump(2);
  }

  std::filesystem::path path_;
};

} // namespace wuwe::agent::reflection

#endif // WUWE_AGENT_REFLECTION_STORE_HPP
