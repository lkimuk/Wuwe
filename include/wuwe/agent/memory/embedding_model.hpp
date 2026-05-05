#ifndef WUWE_AGENT_MEMORY_EMBEDDING_MODEL_HPP
#define WUWE_AGENT_MEMORY_EMBEDDING_MODEL_HPP

#include <string_view>
#include <string>
#include <vector>

namespace wuwe::agent::memory {

class embedding_model {
public:
  virtual ~embedding_model() = default;

  virtual std::vector<float> embed(std::string_view text) const = 0;

  virtual std::vector<std::vector<float>> embed_batch(
    const std::vector<std::string>& texts) const {
    std::vector<std::vector<float>> result;
    result.reserve(texts.size());
    for (const auto& text : texts) {
      result.push_back(embed(text));
    }
    return result;
  }
};

} // namespace wuwe::agent::memory

#endif // WUWE_AGENT_MEMORY_EMBEDDING_MODEL_HPP
