#include <algorithm>

#include "ui/terminal/view.h"
#include "view_internal.h"

namespace manbo {
namespace {
auto
Fold(std::string text) -> std::string {
  for (auto& c : text)
    if (c >= 'A' && c <= 'Z')
      c = static_cast<char>(c - 'A' + 'a');
  return text;
}
}  // namespace

namespace render_internal {
auto
FindRenderedText(const std::vector<StyledLine>& lines, std::string_view query)
  -> std::vector<TextMatch> {
  std::vector<TextMatch> matches;
  if (query.empty())
    return matches;
  const auto needle = Fold(SafeText(query));
  for (std::size_t row = 0; row < lines.size(); ++row) {
    std::string text;
    for (const auto& span : lines[row])
      text += SafeText(span.text);
    text = Fold(std::move(text));
    for (auto offset = text.find(needle); offset != text.npos;
      offset = text.find(needle, offset + needle.size()))
      matches.push_back({row, offset, needle.size()});
  }
  return matches;
}

auto
HighlightMatches(StyledLine line,
  const std::vector<TextMatch>& matches,
  std::size_t row,
  std::optional<TextMatch> current) -> StyledLine {
  const auto first =
    std::ranges::lower_bound(matches, row, {}, &TextMatch::line);
  if (first == matches.end() || first->line != row)
    return line;
  StyledLine result;
  std::size_t offset = 0;
  auto match = first;
  for (auto span : line) {
    span.text = SafeText(span.text);
    std::size_t start = 0;
    while (start < span.text.size()) {
      while (match != matches.end() && match->line == row &&
             match->byte + match->length <= offset + start)
        ++match;
      const bool active = match != matches.end() && match->line == row;
      const bool inside = active && match->byte <= offset + start;
      const auto stop =
        !active
          ? span.text.size()
          : std::min(span.text.size(),
              (inside ? match->byte + match->length : match->byte) - offset);
      auto part = span;
      part.text = span.text.substr(start, stop - start);
      if (inside)
        part.role = current && *current == *match ? StyleRole::kCurrentMatch
                                                  : StyleRole::kMatch;
      result.push_back(std::move(part));
      start = stop;
    }
    offset += span.text.size();
  }
  return result;
}
}  // namespace render_internal

auto
ReaderTextWidth(int viewport_width) -> int {
  return std::clamp(viewport_width - 2, 4, 104);
}

auto
FindDocumentText(
  const Document& document, int viewport_width, std::string_view query)
  -> std::vector<TextMatch> {
  return render_internal::FindRenderedText(
    render_internal::LayoutDocument(document, ReaderTextWidth(viewport_width))
      .lines,
    query);
}
}  // namespace manbo
