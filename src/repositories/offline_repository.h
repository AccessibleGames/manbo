#ifndef MANBO_OFFLINE_REPOSITORY_H_
#define MANBO_OFFLINE_REPOSITORY_H_

#include <filesystem>
#include <memory>
#include <vector>

#include "core/interfaces.h"

namespace manbo {
class CatalogIndex;
// A prepared SQLite index reads matching metadata on demand. Small or stale
// catalogs fall back to JSON; page bodies are always loaded only by Read.
// There is no networking, rendering, import rewriting or executable content.
class OfflineRepository final : public IDocumentRepository {
 public:
  ~OfflineRepository() override;
  static Result<std::unique_ptr<OfflineRepository>>
  Open(const std::filesystem::path& directory);
  Result<std::vector<SearchHit>>
  Search(std::string_view query, std::stop_token stop) override;
  Result<Document>
  Read(std::string_view page_id, std::stop_token stop) override;
  Result<LinkedDocument>
  Follow(std::string_view page_id,
    std::string_view target,
    std::stop_token stop) override;

 private:
  OfflineRepository();

  struct Entry {
    Document metadata;
    std::filesystem::path file;
    std::string search_title;
    std::string search_label;
    std::string search_text;
  };
  Result<Document>
  Load(const Entry& entry, std::stop_token stop) const;
  Result<Entry>
  Lookup(std::string_view reference, std::stop_token stop) const;
  Result<Entry>
  FindSource(std::string_view value, bool by_file, std::stop_token stop) const;
  std::filesystem::path root_;
  std::vector<Entry> entries_;
  std::unique_ptr<CatalogIndex> index_;
};
}  // namespace manbo

#endif  // MANBO_OFFLINE_REPOSITORY_H_
