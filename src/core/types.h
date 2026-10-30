#ifndef MANBO_TYPES_H_
#define MANBO_TYPES_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "core/result.h"
#include "documents/content.h"

namespace manbo {

using RequestId = std::uint64_t;

// These are documentation values, not terminal widgets or framework objects.
struct Document {
  std::string id;
  std::string title;
  std::string manual_section;
  std::string description;
  std::string body;  // Immutable original source, never decorated with ANSI.
  std::string source;
  SourceFormat format = SourceFormat::kPlainText;
  std::shared_ptr<const RichDocument> content = nullptr;
  SourceFamily family = SourceFamily::kGeneric;
  std::string source_url = {};
  std::filesystem::path source_file =
    {};  // Optional local original, never output.
};

struct SearchHit {
  std::string id;
  std::string title;
  std::string manual_section;
  std::string description;
  std::string source;
};

struct Search {
  std::string query;
};
struct ShowSearch {};
struct Select {
  int delta;
};
struct OpenSelected {};
struct OpenPage {
  std::string
    reference;  // ID or an unambiguous name(section), never a shell command.
};
struct FollowLink {
  std::string target;
};
struct Back {};
struct Scroll {
  int lines;
  std::optional<std::size_t> max_offset = std::nullopt;
  std::optional<std::size_t> from_offset = std::nullopt;
};
struct ScrollTo {
  std::size_t line;
  std::size_t max_offset;
};
struct Refresh {};
struct Quit {};

struct SearchRequest {
  RequestId id;
  std::string query;
};
struct ReadRequest {
  RequestId id;
  std::string page_id;
  bool preview = false;
};
struct LinkRequest {
  RequestId id;
  std::string page_id;
  std::string target;
};
using Request = std::variant<SearchRequest, ReadRequest, LinkRequest>;

struct LinkedDocument {
  Document document;
  std::string anchor;
};

struct SearchCompleted {
  RequestId id;
  std::vector<SearchHit> hits;
};
struct ReadCompleted {
  RequestId id;
  Document document;
  bool preview = false;
};
struct JobFailed {
  RequestId id;
  Error error;
};
struct LinkCompleted {
  RequestId id;
  LinkedDocument page;
};
using Event = std::variant<Search,
  ShowSearch,
  Select,
  OpenSelected,
  OpenPage,
  FollowLink,
  Back,
  Scroll,
  ScrollTo,
  Refresh,
  Quit,
  SearchCompleted,
  ReadCompleted,
  LinkCompleted,
  JobFailed>;

enum class Route { kResults, kReader };

struct Snapshot {
  Route route = Route::kResults;
  std::string query;
  std::vector<SearchHit> hits;
  std::size_t selection = 0;
  std::optional<Document> document;
  std::optional<Document> preview;
  bool preview_pending = false;
  std::string preview_error;
  std::size_t scroll_line = 0;
  RequestId generation = 0;
  bool pending = false;
  bool running = true;
  std::string status;
  bool search_home = false;
  std::string document_anchor = {};
};

struct Transition {
  bool visible_change = false;
  bool cancel_work = false;
  std::optional<Request> request;
};

struct SessionMetrics {
  std::uint64_t events = 0;
  std::uint64_t waits = 0;
  std::uint64_t renders = 0;
  std::uint64_t flushes = 0;
  std::uint64_t requests = 0;
};

inline RequestId
GetRequestId(const Request& request) {
  return std::visit([](const auto& value) { return value.id; }, request);
}

}  // namespace manbo

#endif  // MANBO_TYPES_H_
