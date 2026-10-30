#ifndef MANBO_MANUAL_REPOSITORY_H_
#define MANBO_MANUAL_REPOSITORY_H_

#include <memory>

#include "core/interfaces.h"

namespace manbo {
// Add the actual bundled manbo(1) manual, even with no external documentation.
// Null backend is a useful manual-only repository, not an error.
auto
WithManboManual(std::unique_ptr<IDocumentRepository> backend = nullptr)
  -> std::unique_ptr<IDocumentRepository>;
}  // namespace manbo

#endif  // MANBO_MANUAL_REPOSITORY_H_
