#ifndef MANBO_REPOSITORIES_CATALOG_RECORD_H_
#define MANBO_REPOSITORIES_CATALOG_RECORD_H_

#include <nlohmann/json_fwd.hpp>

#include "core/types.h"

namespace manbo::catalog_internal {
struct CatalogRecord {
  Document metadata;
  std::filesystem::path file;
};

bool
Within(const std::filesystem::path& catalog_root,
  const std::filesystem::path& source_path);

Result<CatalogRecord>
DecodeRecord(
  const nlohmann::json& record, const std::filesystem::path& catalog_root);
}  // namespace manbo::catalog_internal

#endif  // MANBO_REPOSITORIES_CATALOG_RECORD_H_
