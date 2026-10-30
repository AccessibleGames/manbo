#ifndef MANBO_SRC_DOCUMENTS_ADAPTERS_GNU_ADAPTER_H_
#define MANBO_SRC_DOCUMENTS_ADAPTERS_GNU_ADAPTER_H_

#include <memory>

#include "documents/content.h"

namespace manbo::documents_internal {
std::unique_ptr<IContentAdapter>
MakeGnuCommandAdapter();
}  // namespace manbo::documents_internal

#endif  // MANBO_SRC_DOCUMENTS_ADAPTERS_GNU_ADAPTER_H_
