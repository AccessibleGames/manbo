#ifndef MANBO_REPOSITORIES_LINK_TARGET_H_
#define MANBO_REPOSITORIES_LINK_TARGET_H_

#include "core/types.h"

namespace manbo {
struct LinkTarget {
  bool same_page = false;
  std::string anchor;
  std::vector<std::filesystem::path> files;
  std::string source_url;
};

// Interpret URI syntax using libxml2. Callers still require a catalog match
// and validate that any selected source stays inside the library.
Result<LinkTarget>
ResolveLink(const Document& origin, std::string_view reference);
}  // namespace manbo

#endif  // MANBO_REPOSITORIES_LINK_TARGET_H_
