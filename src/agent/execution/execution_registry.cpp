#include <wuwe/agent/execution/execution_registry.hpp>

#include <utility>

#include <wuwe/agent/execution/controlled_process_backend.hpp>

namespace wuwe::agent::execution {

void execution_backend_registry::register_backend(std::string name, factory create) {
  entries_.push_back({ .name = std::move(name), .create = std::move(create) });
}

std::unique_ptr<execution_backend> execution_backend_registry::create(
  const std::string& name) const {
  for (const auto& entry : entries_) {
    if (entry.name == name) {
      return entry.create();
    }
  }
  return nullptr;
}

std::vector<sandbox::sandbox_backend_info> execution_backend_registry::backends() const {
  std::vector<sandbox::sandbox_backend_info> result;
  result.reserve(entries_.size());
  for (const auto& entry : entries_) {
    if (auto backend = entry.create()) {
      result.push_back(backend->info());
    }
  }
  return result;
}

execution_backend_registry make_default_execution_backend_registry() {
  execution_backend_registry registry;
  registry.register_backend("controlled_process", [] {
    return make_controlled_process_backend();
  });
  return registry;
}

} // namespace wuwe::agent::execution
