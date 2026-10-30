#include "repositories/manual_repository.h"

#include <algorithm>
#include <cctype>
#include <ranges>

#include "manual_data.h"

namespace manbo {
namespace {
constexpr std::string_view kId = "builtin/manbo/1";
auto
Lower(std::string_view text) -> std::string {
  std::string result(text);
  std::ranges::transform(result, result.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return result;
}

auto
Hit() -> SearchHit {
  return {std::string(kId),
    "manbo",
    "1",
    "Offline documentation browser command manual",
    "Manbo built-in manual"};
}
class ManualRepository final : public IDocumentRepository {
 public:
  explicit ManualRepository(std::unique_ptr<IDocumentRepository> backend)
      : backend_(std::move(backend)) {
  }

  auto
  Search(std::string_view query, std::stop_token stop)
    -> Result<std::vector<SearchHit>> override {
    if (stop.stop_requested())
      return tl::make_unexpected(
        Error{ErrorCode::kCancelled, "Search cancelled"});
    Result<std::vector<SearchHit>> hits =
      backend_ ? backend_->Search(query, stop) : std::vector<SearchHit>{};
    if (!hits)
      return hits;
    const auto needle = Lower(query);
    constexpr std::string_view haystack =
      "manbo manbo(1) builtin/manbo/1 offline documentation browser command "
      "manual help usage man less commands";
    bool all_words = true;
    std::size_t start = 0;
    while (start < needle.size()) {
      start = needle.find_first_not_of(' ', start);
      if (start == std::string::npos)
        break;
      const auto end = needle.find(' ', start);
      if (haystack.find(needle.substr(start, end - start)) == haystack.npos) {
        all_words = false;
        break;
      }
      start = end == std::string::npos ? needle.size() : end + 1;
    }
    if (all_words)
      hits->insert(hits->begin(), Hit());
    return hits;
  }

  auto
  Read(std::string_view page, std::stop_token stop)
    -> Result<Document> override {
    if (stop.stop_requested())
      return tl::make_unexpected(
        Error{ErrorCode::kCancelled, "Read cancelled"});
    if (page != kId && page != "manbo" && page != "manbo(1)") {
      if (backend_)
        return backend_->Read(page, stop);
      return tl::make_unexpected(Error{ErrorCode::kNotFound,
        "No such offline page; use manbo manbo for setup"});
    }
    Document document{std::string(kId),
      "manbo",
      "1",
      Hit().description,
      std::string(kManboManual),
      Hit().source};
    auto content = AdaptContent({document.body, document.format, "sh"}, stop);
    if (!content)
      return tl::make_unexpected(content.error());
    document.content = std::move(*content);
    return document;
  }

  Result<LinkedDocument>
  Follow(std::string_view page_id,
    std::string_view target,
    std::stop_token stop) override {
    if (backend_ && page_id != kId)
      return backend_->Follow(page_id, target, stop);
    return IDocumentRepository::Follow(page_id, target, stop);
  }

 private:
  std::unique_ptr<IDocumentRepository> backend_;
};
}  // namespace

auto
WithManboManual(std::unique_ptr<IDocumentRepository> backend)
  -> std::unique_ptr<IDocumentRepository> {
  return std::make_unique<ManualRepository>(std::move(backend));
}
}  // namespace manbo
