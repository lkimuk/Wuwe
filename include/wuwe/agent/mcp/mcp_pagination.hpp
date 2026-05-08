#ifndef WUWE_AGENT_MCP_PAGINATION_HPP
#define WUWE_AGENT_MCP_PAGINATION_HPP

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

namespace wuwe::agent::mcp {

using json = nlohmann::json;

class mcp_list_paginator {
public:
  explicit mcp_list_paginator(std::size_t page_size = 0) : page_size_(page_size) {
  }

  json page(std::string_view key, json items, const json& params) const {
    const auto result_key = std::string(key);
    if (page_size_ == 0 || items.size() <= page_size_) {
      return { { result_key, std::move(items) } };
    }

    const auto start = cursor_offset(params);
    json page_items = json::array();
    std::size_t index = start;
    while (index < items.size() && page_items.size() < page_size_) {
      page_items.push_back(std::move(items[index]));
      ++index;
    }

    json result { { result_key, std::move(page_items) } };
    if (index < items.size()) {
      result["nextCursor"] = std::to_string(index);
    }
    return result;
  }

  static std::size_t cursor_offset(const json& params) {
    if (!params.is_object()) {
      return 0;
    }
    const auto cursor = params.find("cursor");
    if (cursor == params.end() || !cursor->is_string()) {
      return 0;
    }

    std::size_t result = 0;
    for (const auto ch : cursor->get<std::string>()) {
      if (ch < '0' || ch > '9') {
        return 0;
      }
      result = result * 10 + static_cast<std::size_t>(ch - '0');
    }
    return result;
  }

private:
  std::size_t page_size_ {};
};

} // namespace wuwe::agent::mcp

#endif // WUWE_AGENT_MCP_PAGINATION_HPP
