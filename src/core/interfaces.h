#ifndef MANBO_INTERFACES_H_
#define MANBO_INTERFACES_H_

#include <optional>
#include <stop_token>
#include <string_view>
#include <utility>

#include "core/types.h"

namespace manbo {

// Backend ABC. Calls may block: the job service, not a view, owns scheduling.
class IDocumentRepository {
 public:
  virtual ~IDocumentRepository() = default;
  virtual Result<std::vector<SearchHit>>
  Search(std::string_view query, std::stop_token stop) = 0;
  virtual Result<Document>
  Read(std::string_view page_id, std::stop_token stop) = 0;
  // Resolve only installed references. Plain repositories can use page names;
  // opening a reference never launches a browser or a process.
  virtual Result<LinkedDocument>
  Follow(std::string_view, std::string_view reference, std::stop_token stop) {
    auto document = Read(reference, stop);
    if (!document)
      return tl::make_unexpected(document.error());
    return LinkedDocument{std::move(*document), {}};
  }
};

class IJobService {
 public:
  virtual ~IJobService() = default;
  // Nonblocking admission. A newer request supersedes obsolete work.
  virtual Result<void>
  Submit(Request request) = 0;
  virtual void
  Cancel() noexcept = 0;
  virtual void
  Shutdown() noexcept = 0;
};

class IEventSource {
 public:
  virtual ~IEventSource() = default;
  // Blocks until actual work arrives. nullopt is normal end-of-stream.
  virtual Result<std::optional<Event>>
  Next() = 0;
};

class IRenderer {
 public:
  virtual ~IRenderer() = default;
  virtual Result<void>
  Render(const Snapshot& snapshot) = 0;
  virtual Result<void>
  Flush() = 0;
};

}  // namespace manbo

#endif  // MANBO_INTERFACES_H_
