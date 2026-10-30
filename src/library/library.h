#ifndef MANBO_LIBRARY_LIBRARY_H_
#define MANBO_LIBRARY_LIBRARY_H_

#include "core/types.h"

namespace manbo {
struct ImportOptions {
  std::filesystem::path source;
  std::filesystem::path pages =
    {};  // Index this subtree; copy the whole source tree.
  std::filesystem::path destination = "data/library";
  std::string pack;
  std::string edition = "local";
  SourceFamily family = SourceFamily::kGeneric;
  SourceFormat format = SourceFormat::kHtml;
  std::string source_url;
};

struct ImportSummary {
  std::size_t imported = 0;
  std::size_t total = 0;
  std::filesystem::path library;
};

Result<ImportSummary>
ImportDocuments(const ImportOptions& options, std::stop_token stop = {});

Result<void>
RebuildCatalogIndex(
  const std::filesystem::path& library, std::stop_token stop = {});
}  // namespace manbo

#endif  // MANBO_LIBRARY_LIBRARY_H_
