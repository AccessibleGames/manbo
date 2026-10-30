#include "repositories/offline_repository.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <nlohmann/json.hpp>
#include <ranges>
#include <unordered_set>

#include "repositories/catalog_index.h"
#include "repositories/catalog_record.h"
#include "repositories/link_target.h"

namespace manbo {
namespace {
using catalog_internal::DecodeRecord;
using catalog_internal::Within;
// Full reference libraries contain tens of thousands of pages. Bound catalog
// memory separately from individual source files, which are loaded on demand.
constexpr std::size_t kCatalogByteLimit = 64 * 1024 * 1024;
constexpr std::size_t kCatalogEntryLimit = 100'000;

std::string
Lower(std::string_view text) {
  std::string lowercase_text(text);
  std::ranges::transform(
    lowercase_text, lowercase_text.begin(), [](unsigned char character) {
      return static_cast<char>(std::tolower(character));
    });
  return lowercase_text;
}

std::string
Label(const Document& document) {
  return document.manual_section.empty()
           ? document.title
           : fmt::format("{}({})", document.title, document.manual_section);
}

Result<std::string>
ReadBytes(const std::filesystem::path& file_path, std::size_t size_limit) {
  if (!std::filesystem::is_regular_file(file_path))
    return tl::make_unexpected(Error{ErrorCode::kNotFound,
      "Offline source file is missing or not a regular file"});
  std::ifstream file(file_path, std::ios::binary);
  if (!file)
    return tl::make_unexpected(
      Error{ErrorCode::kNotFound, "Could not open offline source"});
  std::string file_bytes;
  std::array<char, 16 * 1024> buffer;
  while (file && file_bytes.size() <= size_limit) {
    const auto chunk_size =
      std::min(buffer.size(), size_limit + 1 - file_bytes.size());
    file.read(buffer.data(), static_cast<std::streamsize>(chunk_size));
    file_bytes.append(buffer.data(), static_cast<std::size_t>(file.gcount()));
  }

  if (file.bad())
    return tl::make_unexpected(
      Error{ErrorCode::kBackend, "Offline source read failed"});
  if (file_bytes.size() > size_limit)
    return tl::make_unexpected(Error{
      ErrorCode::kInvalidArgument, "Offline file exceeds its size limit"});
  return file_bytes;
}


Error
Cancelled() {
  return {ErrorCode::kCancelled, "Offline operation cancelled"};
}
}  // namespace

OfflineRepository::OfflineRepository() = default;

OfflineRepository::~OfflineRepository() = default;

Result<std::unique_ptr<OfflineRepository>>
OfflineRepository::Open(const std::filesystem::path& catalog_directory) {
  try {
    auto repository =
      std::unique_ptr<OfflineRepository>(new OfflineRepository());
    repository->root_ = std::filesystem::weakly_canonical(catalog_directory);
    auto index = CatalogIndex::Open(repository->root_);
    if (index) {
      repository->index_ = std::move(*index);
      return repository;
    }
    if (index.error().code != ErrorCode::kNotFound)
      return tl::make_unexpected(index.error());

    auto catalog_bytes =
      ReadBytes(repository->root_ / "catalog.json", kCatalogByteLimit);
    if (!catalog_bytes)
      return tl::make_unexpected(catalog_bytes.error());
    const auto catalog_json = nlohmann::json::parse(*catalog_bytes);
    if (catalog_json.at("schema_version") != 1 ||
        !catalog_json.at("documents").is_array() ||
        catalog_json.at("documents").size() > kCatalogEntryLimit)
      return tl::make_unexpected(Error{ErrorCode::kInvalidArgument,
        "Unsupported or oversized offline catalog"});
    std::unordered_set<std::string> ids;
    repository->entries_.reserve(catalog_json.at("documents").size());

    for (const auto& catalog_entry : catalog_json.at("documents")) {
      auto record = DecodeRecord(catalog_entry, repository->root_);
      if (!record)
        return tl::make_unexpected(record.error());
      if (!ids.insert(record->metadata.id).second)
        return tl::make_unexpected(
          Error{ErrorCode::kInvalidArgument, "Duplicate offline document ID"});
      Entry entry{
        std::move(record->metadata), std::move(record->file), {}, {}, {}};
      entry.search_title = Lower(entry.metadata.title);
      entry.search_label = Lower(Label(entry.metadata));
      entry.search_text = Lower(fmt::format("{} {} {} {} {}",
        entry.metadata.id,
        entry.search_label,
        entry.metadata.description,
        entry.metadata.source,
        entry.metadata.manual_section));
      repository->entries_.push_back(std::move(entry));
    }
    return repository;
  } catch (const nlohmann::json::exception& error) {
    return tl::make_unexpected(Error{ErrorCode::kInvalidArgument,
      std::string("Invalid offline catalog: ") + error.what()});
  } catch (const std::exception& error) {
    return tl::make_unexpected(Error{ErrorCode::kBackend, error.what()});
  }
}

Result<std::vector<SearchHit>>
OfflineRepository::Search(std::string_view query, std::stop_token stop) {
  if (stop.stop_requested())
    return tl::make_unexpected(Cancelled());
  if (index_)
    return index_->Search(query, stop);

  const auto needle = Lower(query);
  struct Hit {
    int score;
    SearchHit hit;
  };
  std::vector<Hit> hits;
  for (const auto& entry : entries_) {
    if (stop.stop_requested())
      return tl::make_unexpected(Cancelled());
    const auto& doc = entry.metadata;
    const auto& title = entry.search_title;
    const auto& haystack = entry.search_text;
    int score = needle.empty() ? 1 : 0;
    if (!needle.empty() && (needle == title || needle == Lower(doc.id) ||
                             needle == entry.search_label))
      score = 1000;
    else if (!needle.empty() && title.starts_with(needle))
      score = 750;
    else if (!needle.empty() && title.find(needle) != std::string::npos)
      score = 600;
    else if (!needle.empty()) {
      bool all = true;
      std::size_t start = 0;
      while (start < needle.size()) {
        start = needle.find_first_not_of(' ', start);
        if (start == std::string::npos)
          break;
        const auto end = needle.find(' ', start);
        if (haystack.find(needle.substr(start, end - start)) ==
            std::string::npos) {
          all = false;
          break;
        }
        start = end == std::string::npos ? needle.size() : end + 1;
      }
      if (all)
        score = 300;
    }
    if (score)
      hits.push_back({score,
        {doc.id, doc.title, doc.manual_section, doc.description, doc.source}});
  }
  std::ranges::stable_sort(
    hits, [](const auto& a, const auto& b) { return a.score > b.score; });
  std::vector<SearchHit> result;
  for (auto& hit : hits)
    result.push_back(std::move(hit.hit));
  return result;
}

Result<Document>
OfflineRepository::Load(const Entry& entry, std::stop_token stop) const {
  try {
    if (stop.stop_requested())
      return tl::make_unexpected(Cancelled());
    if (!Within(root_, entry.file))
      return tl::make_unexpected(Error{ErrorCode::kInvalidArgument,
        "Offline source path escapes its catalog"});
    auto bytes = ReadBytes(entry.file, 2 * 1024 * 1024);
    if (!bytes)
      return tl::make_unexpected(bytes.error());
    auto doc = entry.metadata;
    doc.source_file = entry.file;
    doc.body = std::move(*bytes);
    const auto language =
      doc.family == SourceFamily::kCppReference                 ? "cpp"
      : doc.manual_section == "1" || doc.manual_section == "1p" ? "sh"
                                                                : "c";
    auto content =
      AdaptContent({doc.body, doc.format, language, doc.family}, stop);
    if (!content)
      return tl::make_unexpected(content.error());
    doc.content = std::move(*content);
    return doc;
  } catch (const std::exception& error) {
    return tl::make_unexpected(Error{ErrorCode::kBackend, error.what()});
  }
}

Result<OfflineRepository::Entry>
OfflineRepository::Lookup(
  std::string_view page_id, std::stop_token stop) const {
  if (stop.stop_requested())
    return tl::make_unexpected(Cancelled());
  if (index_) {
    auto metadata = index_->Find(page_id, stop);
    if (!metadata)
      return tl::make_unexpected(metadata.error());
    auto record = DecodeRecord(*metadata, root_);
    if (!record)
      return tl::make_unexpected(record.error());
    Entry entry{
      std::move(record->metadata), std::move(record->file), {}, {}, {}};
    return entry;
  }

  const Entry* match = nullptr;
  for (const auto& entry : entries_) {
    if (stop.stop_requested())
      return tl::make_unexpected(Cancelled());
    if (entry.metadata.id == page_id)
      return entry;
    if (entry.metadata.title == page_id || Label(entry.metadata) == page_id) {
      if (match)
        return tl::make_unexpected(Error{ErrorCode::kAmbiguous,
          "Ambiguous page name; specify name(section) or a page ID"});
      match = &entry;
    }
  }
  if (!match)
    return tl::make_unexpected(
      Error{ErrorCode::kNotFound, "No such offline page"});
  return *match;
}

Result<Document>
OfflineRepository::Read(std::string_view page_id, std::stop_token stop) {
  auto entry = Lookup(page_id, stop);
  if (!entry)
    return tl::make_unexpected(entry.error());
  return Load(*entry, stop);
}

Result<OfflineRepository::Entry>
OfflineRepository::FindSource(
  std::string_view value, bool by_file, std::stop_token stop) const {
  if (stop.stop_requested())
    return tl::make_unexpected(Cancelled());
  if (index_) {
    auto metadata = index_->FindSource(value, by_file, stop);
    if (!metadata)
      return tl::make_unexpected(metadata.error());
    auto record = DecodeRecord(*metadata, root_);
    if (!record)
      return tl::make_unexpected(record.error());
    return Entry{
      std::move(record->metadata), std::move(record->file), {}, {}, {}};
  }

  const Entry* match = nullptr;
  for (const auto& entry : entries_) {
    if (stop.stop_requested())
      return tl::make_unexpected(Cancelled());
    const auto candidate =
      by_file ? entry.file.lexically_relative(root_).generic_string()
              : entry.metadata.source_url;
    if (candidate != value)
      continue;
    if (match)
      return tl::make_unexpected(Error{
        ErrorCode::kAmbiguous, "Reference matches multiple installed pages"});
    match = &entry;
  }
  if (!match)
    return tl::make_unexpected(Error{ErrorCode::kNotFound,
      "This reference is not available in the offline library"});
  return *match;
}

Result<LinkedDocument>
OfflineRepository::Follow(
  std::string_view page_id, std::string_view reference, std::stop_token stop) {
  try {
    auto origin = Lookup(page_id, stop);
    if (!origin)
      return tl::make_unexpected(origin.error());
    if (origin->metadata.format == SourceFormat::kPlainText) {
      auto page = Read(reference, stop);
      if (!page)
        return tl::make_unexpected(page.error());
      return LinkedDocument{std::move(*page), {}};
    }

    origin->metadata.source_file = origin->file;
    auto target = ResolveLink(origin->metadata, reference);
    if (!target)
      return tl::make_unexpected(target.error());
    if (target->same_page) {
      auto page = Load(*origin, stop);
      if (!page)
        return tl::make_unexpected(page.error());
      return LinkedDocument{std::move(*page), std::move(target->anchor)};
    }

    const auto load_target = [&](const Entry& entry) -> Result<LinkedDocument> {
      auto page = Load(entry, stop);
      if (!page)
        return tl::make_unexpected(page.error());
      return LinkedDocument{std::move(*page), target->anchor};
    };
    for (const auto& file : target->files) {
      if (!Within(root_, file))
        return tl::make_unexpected(Error{ErrorCode::kInvalidArgument,
          "Reference escapes its offline catalog"});
      auto entry =
        FindSource(file.lexically_relative(root_).generic_string(), true, stop);
      if (entry)
        return load_target(*entry);
      if (entry.error().code != ErrorCode::kNotFound)
        return tl::make_unexpected(entry.error());
    }
    if (!target->source_url.empty()) {
      auto entry = FindSource(target->source_url, false, stop);
      if (entry)
        return load_target(*entry);
      return tl::make_unexpected(entry.error());
    }
    return tl::make_unexpected(Error{ErrorCode::kNotFound,
      "This reference is not available in the offline library"});
  } catch (const std::exception& error) {
    return tl::make_unexpected(Error{ErrorCode::kBackend, error.what()});
  }
}
}  // namespace manbo
