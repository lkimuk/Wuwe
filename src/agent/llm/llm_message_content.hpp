#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <wuwe/agent/llm/llm_types.h>

namespace wuwe::agent::llm::detail {

[[nodiscard]] inline std::string base64_encode(const std::vector<std::uint8_t>& bytes) {
  static constexpr char alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string encoded;
  encoded.reserve(((bytes.size() + 2) / 3) * 4);
  for (std::size_t index = 0; index < bytes.size(); index += 3) {
    const unsigned value = (static_cast<unsigned>(bytes[index]) << 16) |
                           ((index + 1 < bytes.size() ? bytes[index + 1] : 0u) << 8) |
                           (index + 2 < bytes.size() ? bytes[index + 2] : 0u);
    encoded.push_back(alphabet[(value >> 18) & 63u]);
    encoded.push_back(alphabet[(value >> 12) & 63u]);
    encoded.push_back(index + 1 < bytes.size() ? alphabet[(value >> 6) & 63u] : '=');
    encoded.push_back(index + 2 < bytes.size() ? alphabet[value & 63u] : '=');
  }
  return encoded;
}

[[nodiscard]] inline nlohmann::json openai_message_content(const ::wuwe::chat_message& message) {
  if (message.images.empty()) {
    return message.content;
  }
  auto parts = nlohmann::json::array();
  if (!message.content.empty()) {
    parts.push_back({ { "type", "text" }, { "text", message.content } });
  }
  for (const auto& image : message.images) {
    parts.push_back({ { "type", "image_url" },
      { "image_url",
        { { "url", "data:" + image.media_type + ";base64," + base64_encode(image.bytes) } } } });
  }
  return parts;
}

[[nodiscard]] inline nlohmann::json anthropic_message_content(const ::wuwe::chat_message& message) {
  if (message.images.empty()) {
    return message.content;
  }
  auto parts = nlohmann::json::array();
  for (const auto& image : message.images) {
    parts.push_back({ { "type", "image" },
      { "source",
        { { "type", "base64" },
          { "media_type", image.media_type },
          { "data", base64_encode(image.bytes) } } } });
  }
  if (!message.content.empty()) {
    parts.push_back({ { "type", "text" }, { "text", message.content } });
  }
  return parts;
}

[[nodiscard]] inline void append_gemini_images(
  nlohmann::json& parts, const ::wuwe::chat_message& message) {
  for (const auto& image : message.images) {
    parts.push_back({ { "inlineData",
      { { "mimeType", image.media_type }, { "data", base64_encode(image.bytes) } } } });
  }
}

[[nodiscard]] inline nlohmann::json ollama_message_images(const ::wuwe::chat_message& message) {
  auto images = nlohmann::json::array();
  for (const auto& image : message.images) {
    images.push_back(base64_encode(image.bytes));
  }
  return images;
}

[[nodiscard]] inline nlohmann::json codex_message_content(const ::wuwe::chat_message& message) {
  auto parts = nlohmann::json::array();
  if (!message.content.empty() || message.images.empty()) {
    parts.push_back({ { "type", message.role == "user" ? "input_text" : "output_text" },
      { "text", message.content } });
  }
  for (const auto& image : message.images) {
    parts.push_back(
      { { "type", "input_image" }, { "image_url", "data:" + image.media_type + ";base64," + base64_encode(image.bytes) } });
  }
  return parts;
}

} // namespace wuwe::agent::llm::detail
