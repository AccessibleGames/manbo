#ifndef MANBO_SRC_DOCUMENTS_ADAPTERS_PLAIN_ADAPTER_H_
#define MANBO_SRC_DOCUMENTS_ADAPTERS_PLAIN_ADAPTER_H_

#include <memory>

#include "documents/content.h"

namespace manbo::documents_internal {
std::unique_ptr<IContentAdapter>
MakeGenericAdapter();
}  // namespace manbo::documents_internal

#endif  // MANBO_SRC_DOCUMENTS_ADAPTERS_PLAIN_ADAPTER_H_
