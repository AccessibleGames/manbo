#ifndef MANBO_REPOSITORIES_CATALOG_INDEX_H_
#define MANBO_REPOSITORIES_CATALOG_INDEX_H_

#include <filesystem>
#include <nlohmann/json_fwd.hpp>

#include "core/interfaces.h"

namespace manbo {
// Private SQLite storage adapter. Connections and statements belong to each
// operation, so concurrent jobs never share mutable SQLite connection state.
class CatalogIndex {
 public:
  static Result<std::unique_ptr<CatalogIndex>>
  Open(const std::filesystem::path& directory);

  static Result<void>
  Rebuild(const std::filesystem::path& directory, std::stop_token stop = {});

  Result<std::vector<SearchHit>>
  Search(std::string_view query, std::stop_token stop) const;

  Result<nlohmann::json>
  Find(std::string_view reference, std::stop_token stop) const;
  Result<nlohmann::json>
  FindSource(std::string_view value, bool by_file, std::stop_token stop) const;

 private:
  std::filesystem::path file_;
};
}  // namespace manbo

#endif  // MANBO_REPOSITORIES_CATALOG_INDEX_H_
