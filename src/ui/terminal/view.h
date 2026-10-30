#ifndef MANBO_TERMINAL_VIEW_H_
#define MANBO_TERMINAL_VIEW_H_

#include <optional>
#include <string_view>

#include "core/types.h"

namespace manbo {

// Semantic theme roles, not ANSI strings or platform/framework objects.
enum class StyleRole {
  kText,
  kMuted,
  kBrand,
  kChrome,
  kBorder,
  kSelected,
  kSelectedMeta,
  kKey,
  kKeyLabel,
  kHeading,
  kCode,
  kKeyword,
  kString,
  kNumber,
  kComment,
  kLink,
  kStrong,
  kNotice,
  kTableHeader,
  kError,
  kMatch,
  kCurrentMatch,
};
struct StyledSpan {
  std::string text;
  StyleRole role = StyleRole::kText;
  bool bold = false;
  bool italic = false;
  bool underline = false;
  std::string link = {};
};
using StyledLine = std::vector<StyledSpan>;
struct FrameCursor {
  int row;
  int column;
};
struct TextMatch {
  std::size_t line;
  std::size_t byte;
  std::size_t length;
  bool
  operator==(const TextMatch&) const = default;
};
enum class InputMode { kNavigation, kCommand, kQuery, kFind };
struct TerminalPrompt {
  InputMode mode = InputMode::kNavigation;
  std::string text = {};
  std::string notice = {};
  std::string find_query = {};
  std::optional<TextMatch> current_match = std::nullopt;
  std::string selected_link = {};
};
struct PageLink {
  std::string target;
  std::string label;
  std::size_t line;
};
struct StyledFrame {
  int width = 0;
  int height = 0;
  std::vector<StyledLine> lines;
  std::size_t reader_scroll_limit = 0;
  std::optional<FrameCursor> cursor;
  int reader_content_height = 0;
  std::vector<TextMatch> find_matches = {};
  std::vector<PageLink> reader_links = {};
  std::size_t reader_offset = 0;
};

std::string
SafeText(std::string_view text);
std::size_t
TextColumns(std::string_view text);
std::string
PageLabel(const SearchHit& hit);
StyledFrame
RenderFrame(const Snapshot& state,
  int width,
  int height,
  const TerminalPrompt& prompt = {});
int
ReaderTextWidth(int viewport_width);
std::vector<TextMatch>
FindDocumentText(
  const Document& document, int viewport_width, std::string_view query);
std::vector<std::string>
PlainLines(const StyledFrame& frame);
// Whole-document text for pipes/review, without browser chrome or ANSI.
std::vector<std::string>
RenderDocumentText(const Document& document, int width);
std::string
SerializeAnsi(const StyledFrame& frame);
// Deterministic, inert visual-review artifact; never embeds source HTML.
std::string
SerializeHtml(const StyledFrame& frame);
std::vector<std::string>
RenderLines(const Snapshot& state, int width, int height);

}  // namespace manbo

#endif  // MANBO_TERMINAL_VIEW_H_
