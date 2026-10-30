#ifndef MANBO_SRC_DOCUMENTS_ADAPTERS_LINUX_MAN_ADAPTER_H_
#define MANBO_SRC_DOCUMENTS_ADAPTERS_LINUX_MAN_ADAPTER_H_

#include <memory>

#include "documents/content.h"

namespace manbo::documents_internal {
std::unique_ptr<IContentAdapter>
MakeLinuxManAdapter();
}  // namespace manbo::documents_internal

#endif  // MANBO_SRC_DOCUMENTS_ADAPTERS_LINUX_MAN_ADAPTER_H_
