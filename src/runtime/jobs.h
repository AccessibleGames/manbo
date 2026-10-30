#ifndef MANBO_JOBS_H_
#define MANBO_JOBS_H_

#include <memory>

#include "core/interfaces.h"
#include "runtime/channel.h"

namespace manbo {

// Zero workers is an inline adapter for small bounded/cooperative backends
// only. Positive counts select a Boost.Asio pool with one latest-wins pending
// request slot.
Result<std::unique_ptr<IJobService>>
MakeJobService(IDocumentRepository& repository,
  EventChannel& replies,
  std::size_t workers = 1);

}  // namespace manbo

#endif  // MANBO_JOBS_H_
