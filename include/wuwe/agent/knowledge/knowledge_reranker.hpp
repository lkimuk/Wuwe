#ifndef WUWE_AGENT_KNOWLEDGE_RERANKER_HPP
#define WUWE_AGENT_KNOWLEDGE_RERANKER_HPP

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cctype>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <wuwe/agent/knowledge/knowledge_record.hpp>
#include <wuwe/net/default_http_client.h>
#include <wuwe/net/http_client.h>

namespace wuwe::agent::knowledge {

class knowledge_reranker {
public:
  virtual ~knowledge_reranker() = default;

  virtual std::vector<knowledge_result> rerank(
    const knowledge_query& query,
    std::vector<knowledge_result> candidates) const = 0;
};

struct score_knowledge_reranker_policy {
  std::size_t max_per_document { 0 };
};

struct bm25_knowledge_reranker_policy {
  double k1 { 1.2 };
  double b { 0.75 };
  double existing_score_weight { 0.35 };
  double bm25_weight { 1.0 };
  std::size_t max_per_document { 0 };
};

struct mmr_knowledge_reranker_policy {
  double relevance_weight { 0.7 };
  double diversity_weight { 0.3 };
  std::size_t max_per_document { 0 };
};

class cross_encoder_knowledge_scorer {
public:
  virtual ~cross_encoder_knowledge_scorer() = default;

  virtual double score(const knowledge_query& query, const knowledge_result& candidate) const = 0;
};

class callback_cross_encoder_knowledge_scorer final : public cross_encoder_knowledge_scorer {
public:
  using callback_type = std::function<double(const knowledge_query&, const knowledge_result&)>;

  explicit callback_cross_encoder_knowledge_scorer(callback_type callback)
      : callback_(std::move(callback)) {
  }

  double score(const knowledge_query& query, const knowledge_result& candidate) const override {
    return callback_ ? callback_(query, candidate) : 0.0;
  }

private:
  callback_type callback_;
};

struct http_cross_encoder_knowledge_scorer_config {
  std::string endpoint_url;
  std::string api_key;
  int timeout_ms { 30000 };
};

class http_cross_encoder_knowledge_scorer final : public cross_encoder_knowledge_scorer {
public:
  http_cross_encoder_knowledge_scorer(
    http_cross_encoder_knowledge_scorer_config config,
    std::shared_ptr<::wuwe::http_client> http = std::make_shared<::wuwe::default_http_client>())
      : config_(std::move(config)), http_(std::move(http)) {
    if (config_.endpoint_url.empty()) {
      throw std::invalid_argument("http_cross_encoder_knowledge_scorer requires endpoint_url");
    }
    if (!http_) {
      throw std::invalid_argument("http_cross_encoder_knowledge_scorer requires http_client");
    }
  }

  double score(const knowledge_query& query, const knowledge_result& candidate) const override {
    nlohmann::json body {
      { "query", query.text },
      { "candidate", {
        { "id", candidate.chunk.id },
        { "document_id", candidate.chunk.document_id },
        { "title", candidate.chunk.title },
        { "content", candidate.chunk.content },
        { "source_uri", candidate.chunk.source_uri },
        { "metadata", candidate.chunk.metadata },
      } },
      { "score", candidate.score },
      { "vector_score", candidate.vector_score },
      { "lexical_score", candidate.lexical_score },
    };

    ::wuwe::http_request request {
      .method = "POST",
      .url = config_.endpoint_url,
      .headers = { { "Content-Type", "application/json" } },
      .body = body.dump(),
      .timeout = config_.timeout_ms,
    };
    if (!config_.api_key.empty()) {
      request.headers.push_back({ "Authorization", "Bearer " + config_.api_key });
    }

    const auto response = http_->send(request);
    if (response.error_code) {
      throw std::runtime_error(
        "http cross encoder scorer request failed: " + response.error_code.message());
    }

    const auto data = nlohmann::json::parse(response.body, nullptr, false);
    if (data.is_number()) {
      return data.get<double>();
    }
    if (data.is_object() && data.contains("score") && data["score"].is_number()) {
      return data["score"].get<double>();
    }
    throw std::runtime_error("http cross encoder scorer expected numeric score response");
  }

private:
  http_cross_encoder_knowledge_scorer_config config_;
  std::shared_ptr<::wuwe::http_client> http_;
};

struct cross_encoder_knowledge_reranker_policy {
  double existing_score_weight { 0.2 };
  double model_score_weight { 1.0 };
  std::size_t max_per_document { 0 };
};

class score_knowledge_reranker final : public knowledge_reranker {
public:
  explicit score_knowledge_reranker(score_knowledge_reranker_policy policy = {})
      : policy_(policy) {
  }

  std::vector<knowledge_result> rerank(
    const knowledge_query& query,
    std::vector<knowledge_result> candidates) const override {
    std::sort(candidates.begin(), candidates.end(), [](const knowledge_result& lhs,
                                                       const knowledge_result& rhs) {
      if (lhs.score != rhs.score) {
        return lhs.score > rhs.score;
      }
      if (lhs.chunk.document_id != rhs.chunk.document_id) {
        return lhs.chunk.document_id < rhs.chunk.document_id;
      }
      return lhs.chunk.start_offset < rhs.chunk.start_offset;
    });

    if (policy_.max_per_document != 0) {
      candidates = cap_per_document(std::move(candidates));
    }

    if (query.limit != 0 && candidates.size() > query.limit) {
      candidates.resize(query.limit);
    }
    return candidates;
  }

private:
  std::vector<knowledge_result> cap_per_document(std::vector<knowledge_result> candidates) const {
    std::vector<knowledge_result> result;
    std::unordered_map<std::string, std::size_t> counts;

    for (auto& candidate : candidates) {
      auto& count = counts[candidate.chunk.document_id];
      if (count >= policy_.max_per_document) {
        continue;
      }
      ++count;
      result.push_back(std::move(candidate));
    }
    return result;
  }

  score_knowledge_reranker_policy policy_;
};

class bm25_knowledge_reranker final : public knowledge_reranker {
public:
  explicit bm25_knowledge_reranker(bm25_knowledge_reranker_policy policy = {})
      : policy_(policy) {
  }

  std::vector<knowledge_result> rerank(
    const knowledge_query& query,
    std::vector<knowledge_result> candidates) const override {
    if (query.text.empty() || candidates.empty()) {
      return candidates;
    }

    const auto query_terms = tokenize(query.text);
    if (query_terms.empty()) {
      return candidates;
    }

    std::vector<std::unordered_map<std::string, std::size_t>> term_frequencies;
    term_frequencies.reserve(candidates.size());
    std::unordered_map<std::string, std::size_t> document_frequency;
    double total_length = 0.0;

    for (const auto& candidate : candidates) {
      auto terms = tokenize(candidate.chunk.title + " " + candidate.chunk.content);
      total_length += static_cast<double>(terms.size());

      std::unordered_map<std::string, std::size_t> frequencies;
      std::unordered_set<std::string> seen;
      for (auto& term : terms) {
        ++frequencies[term];
        seen.insert(std::move(term));
      }
      for (const auto& term : seen) {
        ++document_frequency[term];
      }
      term_frequencies.push_back(std::move(frequencies));
    }

    const auto average_length =
      total_length == 0.0 ? 1.0 : total_length / static_cast<double>(candidates.size());
    const auto corpus_size = static_cast<double>(candidates.size());

    for (std::size_t index = 0; index < candidates.size(); ++index) {
      double bm25 = 0.0;
      double document_length = 0.0;
      for (const auto& [_, count] : term_frequencies[index]) {
        document_length += static_cast<double>(count);
      }
      if (document_length == 0.0) {
        document_length = 1.0;
      }

      for (const auto& term : query_terms) {
        const auto frequency_it = term_frequencies[index].find(term);
        if (frequency_it == term_frequencies[index].end()) {
          continue;
        }

        const auto df_it = document_frequency.find(term);
        const auto df = df_it == document_frequency.end() ? 0.0 : static_cast<double>(df_it->second);
        const auto idf = std::log(1.0 + ((corpus_size - df + 0.5) / (df + 0.5)));
        const auto tf = static_cast<double>(frequency_it->second);
        const auto denominator =
          tf + policy_.k1 * (1.0 - policy_.b + policy_.b * (document_length / average_length));
        bm25 += idf * ((tf * (policy_.k1 + 1.0)) / denominator);
      }

      candidates[index].lexical_score = bm25;
      candidates[index].score =
        policy_.existing_score_weight * candidates[index].score +
        policy_.bm25_weight * bm25;
    }

    std::sort(candidates.begin(), candidates.end(), [](const knowledge_result& lhs,
                                                       const knowledge_result& rhs) {
      if (lhs.score != rhs.score) {
        return lhs.score > rhs.score;
      }
      if (lhs.chunk.document_id != rhs.chunk.document_id) {
        return lhs.chunk.document_id < rhs.chunk.document_id;
      }
      return lhs.chunk.start_offset < rhs.chunk.start_offset;
    });

    if (policy_.max_per_document != 0) {
      candidates = cap_per_document(std::move(candidates));
    }
    if (query.limit != 0 && candidates.size() > query.limit) {
      candidates.resize(query.limit);
    }
    return candidates;
  }

private:
  static std::vector<std::string> tokenize(const std::string& text) {
    std::vector<std::string> result;
    std::string current;
    for (const unsigned char ch : text) {
      if (std::isalnum(ch)) {
        current.push_back(static_cast<char>(std::tolower(ch)));
      }
      else if (!current.empty()) {
        result.push_back(std::move(current));
        current.clear();
      }
    }
    if (!current.empty()) {
      result.push_back(std::move(current));
    }
    return result;
  }

  std::vector<knowledge_result> cap_per_document(std::vector<knowledge_result> candidates) const {
    std::vector<knowledge_result> result;
    std::unordered_map<std::string, std::size_t> counts;

    for (auto& candidate : candidates) {
      auto& count = counts[candidate.chunk.document_id];
      if (count >= policy_.max_per_document) {
        continue;
      }
      ++count;
      result.push_back(std::move(candidate));
    }
    return result;
  }

  bm25_knowledge_reranker_policy policy_;
};

class mmr_knowledge_reranker final : public knowledge_reranker {
public:
  explicit mmr_knowledge_reranker(mmr_knowledge_reranker_policy policy = {})
      : policy_(policy) {
  }

  std::vector<knowledge_result> rerank(
    const knowledge_query& query,
    std::vector<knowledge_result> candidates) const override {
    if (candidates.empty()) {
      return candidates;
    }

    std::vector<knowledge_result> selected;
    selected.reserve(query.limit == 0 ? candidates.size() : query.limit);
    std::unordered_map<std::string, std::size_t> per_document_count;

    while (!candidates.empty() && (query.limit == 0 || selected.size() < query.limit)) {
      auto best = candidates.begin();
      double best_score = -std::numeric_limits<double>::infinity();

      for (auto it = candidates.begin(); it != candidates.end(); ++it) {
        if (policy_.max_per_document != 0 &&
            per_document_count[it->chunk.document_id] >= policy_.max_per_document) {
          continue;
        }

        double max_similarity = 0.0;
        for (const auto& item : selected) {
          max_similarity = (std::max)(max_similarity, text_similarity(it->chunk, item.chunk));
        }
        const auto mmr_score =
          policy_.relevance_weight * it->score - policy_.diversity_weight * max_similarity;
        if (mmr_score > best_score) {
          best_score = mmr_score;
          best = it;
        }
      }

      if (best_score == -std::numeric_limits<double>::infinity()) {
        break;
      }
      best->score = best_score;
      ++per_document_count[best->chunk.document_id];
      selected.push_back(std::move(*best));
      candidates.erase(best);
    }
    return selected;
  }

private:
  static double text_similarity(const knowledge_chunk& lhs, const knowledge_chunk& rhs) {
    const auto lhs_tokens = token_set(lhs.title + " " + lhs.content);
    const auto rhs_tokens = token_set(rhs.title + " " + rhs.content);
    if (lhs_tokens.empty() || rhs_tokens.empty()) {
      return 0.0;
    }

    std::size_t intersection = 0;
    for (const auto& token : lhs_tokens) {
      if (rhs_tokens.contains(token)) {
        ++intersection;
      }
    }
    const auto union_size = lhs_tokens.size() + rhs_tokens.size() - intersection;
    return union_size == 0 ? 0.0 : static_cast<double>(intersection) / static_cast<double>(union_size);
  }

  static std::unordered_set<std::string> token_set(const std::string& text) {
    std::unordered_set<std::string> result;
    std::string current;
    for (const unsigned char ch : text) {
      if (std::isalnum(ch)) {
        current.push_back(static_cast<char>(std::tolower(ch)));
      }
      else if (!current.empty()) {
        result.insert(std::move(current));
        current.clear();
      }
    }
    if (!current.empty()) {
      result.insert(std::move(current));
    }
    return result;
  }

  mmr_knowledge_reranker_policy policy_;
};

class cross_encoder_knowledge_reranker final : public knowledge_reranker {
public:
  cross_encoder_knowledge_reranker(
    std::shared_ptr<cross_encoder_knowledge_scorer> scorer,
    cross_encoder_knowledge_reranker_policy policy = {})
      : scorer_(std::move(scorer)), policy_(policy) {
    if (!scorer_) {
      throw std::invalid_argument("cross_encoder_knowledge_reranker requires a scorer");
    }
  }

  std::vector<knowledge_result> rerank(
    const knowledge_query& query,
    std::vector<knowledge_result> candidates) const override {
    for (auto& candidate : candidates) {
      const auto model_score = scorer_->score(query, candidate);
      candidate.lexical_score = model_score;
      candidate.score =
        policy_.existing_score_weight * candidate.score +
        policy_.model_score_weight * model_score;
    }

    std::sort(candidates.begin(), candidates.end(), [](const knowledge_result& lhs,
                                                       const knowledge_result& rhs) {
      if (lhs.score != rhs.score) {
        return lhs.score > rhs.score;
      }
      if (lhs.chunk.document_id != rhs.chunk.document_id) {
        return lhs.chunk.document_id < rhs.chunk.document_id;
      }
      return lhs.chunk.start_offset < rhs.chunk.start_offset;
    });

    if (policy_.max_per_document != 0) {
      candidates = cap_per_document(std::move(candidates));
    }
    if (query.limit != 0 && candidates.size() > query.limit) {
      candidates.resize(query.limit);
    }
    return candidates;
  }

private:
  std::vector<knowledge_result> cap_per_document(std::vector<knowledge_result> candidates) const {
    std::vector<knowledge_result> result;
    std::unordered_map<std::string, std::size_t> counts;
    for (auto& candidate : candidates) {
      auto& count = counts[candidate.chunk.document_id];
      if (count >= policy_.max_per_document) {
        continue;
      }
      ++count;
      result.push_back(std::move(candidate));
    }
    return result;
  }

  std::shared_ptr<cross_encoder_knowledge_scorer> scorer_;
  cross_encoder_knowledge_reranker_policy policy_;
};

} // namespace wuwe::agent::knowledge

#endif // WUWE_AGENT_KNOWLEDGE_RERANKER_HPP
