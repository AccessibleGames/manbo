#ifndef MANBO_UI_TERMINAL_VIEW_INTERNAL_H_
#define MANBO_UI_TERMINAL_VIEW_INTERNAL_H_

#include "ui/terminal/view.h"

namespace manbo::render_internal {

void
Append(StyledLine& line,
  std::string text,
  StyleRole role = StyleRole::kText,
  bool bold = false,
  bool italic = false,
  bool underline = false,
  std::string link = {});
std::size_t
Columns(const StyledLine& line);
StyledLine
Fit(StyledLine line, int width, StyleRole padding = StyleRole::kText);
std::vector<StyledLine>
WrapText(const RichText& text, int width);
std::vector<StyledLine>
WrapPreformatted(const RichText& text, int width);
std::vector<StyledLine>
LayoutTable(const ContentBlock& table,
  int width,
  std::vector<std::size_t>* row_offsets = nullptr);
std::string
Clip(std::string_view text, int width);
struct OutlineItem {
  std::string title;
  std::size_t line;
  unsigned level;
};
struct DocumentLayout {
  std::vector<StyledLine> lines;
  std::vector<OutlineItem> outline;
  struct Anchor {
    std::string name;
    std::size_t line;
  };
  std::vector<Anchor> anchors;
};
DocumentLayout
LayoutDocument(const Document& document, int width);
std::vector<TextMatch>
FindRenderedText(const std::vector<StyledLine>& lines, std::string_view query);
StyledLine
HighlightMatches(StyledLine line,
  const std::vector<TextMatch>& matches,
  std::size_t row,
  std::optional<TextMatch> current);

}  // namespace manbo::render_internal

#endif  // MANBO_UI_TERMINAL_VIEW_INTERNAL_H_
