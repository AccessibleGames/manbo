#include <fmt/format.h>

#include <algorithm>
#include <unordered_map>

#include "ui/terminal/view.h"
#include "view_internal.h"

namespace manbo {
namespace {
using render_internal::Append;
using render_internal::Clip;
using render_internal::Fit;
using render_internal::LayoutDocument;

auto
Place(StyledFrame& frame, int row, StyledLine line) -> void {
  if (row >= 0 && row < frame.height)
    frame.lines[static_cast<std::size_t>(row)] =
      Fit(std::move(line), frame.width);
}

auto
Label(std::string_view title, std::string_view section) -> std::string {
  return section.empty() ? std::string(title)
                         : fmt::format("{}({})", title, section);
}

auto
Join(StyledFrame& frame,
  int row,
  StyledLine left,
  int left_width,
  StyledLine right) -> void {
  auto line = Fit(std::move(left), left_width);
  Append(line, "  ");
  line.insert(line.end(), right.begin(), right.end());
  Place(frame, row, std::move(line));
}

auto
PromptLine(StyledFrame& frame, const TerminalPrompt& prompt, std::string status)
  -> void {
  if (prompt.mode == InputMode::kNavigation) {
    Place(frame,
      frame.height - 1,
      {{prompt.notice.empty() ? std::move(status) : prompt.notice,
        StyleRole::kMuted}});
    return;
  }
  const std::string prefix = prompt.mode == InputMode::kCommand ? ":"
                             : prompt.mode == InputMode::kFind  ? "/"
                                                                : "Search: ";
  auto text = SafeText(prompt.text);
  const auto room =
    std::max(0, frame.width - static_cast<int>(TextColumns(prefix)) - 2);
  bool clipped = false;
  while (!text.empty() &&
         TextColumns(text) > static_cast<std::size_t>(std::max(0, room - 1))) {
    auto end = std::size_t{1};
    while (end < text.size() &&
           (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80)
      ++end;
    text.erase(0, end);
    clipped = true;
  }
  if (clipped)
    text = "…" + text;
  Place(frame, frame.height - 1, {{prefix, StyleRole::kStrong, true}, {text}});
  frame.cursor = FrameCursor{frame.height - 1,
    std::min(frame.width - 2, static_cast<int>(TextColumns(prefix + text)))};
}
}  // namespace

auto
PageLabel(const SearchHit& hit) -> std::string {
  return Label(hit.title, hit.manual_section);
}

auto
RenderFrame(
  const Snapshot& state, int width, int height, const TerminalPrompt& prompt)
  -> StyledFrame {
  StyledFrame frame;
  frame.width = std::clamp(width, 1, 240);
  frame.height = std::clamp(height, 1, 100);
  frame.lines.resize(static_cast<std::size_t>(frame.height));
  width = frame.width;
  height = frame.height;
  if (width < 40 || height < 10) {
    Place(frame,
      0,
      {{"Terminal too small: resize to 40×10 or use the CLI.",
        StyleRole::kNotice}});
    return frame;
  }
  std::string status = state.status;
  if (state.search_home) {
    Place(frame,
      0,
      {{"manbo", StyleRole::kBrand, true},
        {"  —  offline documentation", StyleRole::kMuted}});
    Place(frame, 3, {{"Search your documentation", StyleRole::kHeading, true}});
    Place(
      frame, 5, {{"C/C++: std::vector  std::sort  malloc", StyleRole::kText}});
    Place(
      frame, 6, {{"POSIX/man: socket  pthread_create  ls", StyleRole::kText}});
    Place(frame,
      8,
      {{"Enter searches; empty lists every page.", StyleRole::kMuted}});
  } else if (state.route == Route::kReader) {
    const auto title = state.document ? Label(state.document->title,
                                          state.document->manual_section)
                                      : "Manbo";
    Place(frame,
      0,
      {{title, StyleRole::kHeading, true},
        {state.document ? "  —  " + state.document->source : "",
          StyleRole::kMuted}});
    auto page = state.document
                  ? LayoutDocument(*state.document, ReaderTextWidth(width))
                  : render_internal::DocumentLayout{};
    frame.reader_content_height = height - 3;
    const auto visible = static_cast<std::size_t>(frame.reader_content_height);
    frame.reader_scroll_limit =
      page.lines.size() > visible ? page.lines.size() - visible : 0;
    auto offset = std::min(state.scroll_line, frame.reader_scroll_limit);
    bool missing_anchor = false;
    if (!state.document_anchor.empty()) {
      const auto anchor = std::ranges::find(page.anchors,
        state.document_anchor,
        &render_internal::DocumentLayout::Anchor::name);
      if (anchor != page.anchors.end())
        offset = std::min(anchor->line, frame.reader_scroll_limit);
      else if (state.document_anchor != "top")
        missing_anchor = true;
    }
    frame.reader_offset = offset;

    std::unordered_map<std::string, std::size_t> link_indices;
    for (std::size_t row = 0; row < page.lines.size(); ++row) {
      for (const auto& span : page.lines[row]) {
        if (span.link.empty())
          continue;
        const auto [entry, inserted] =
          link_indices.emplace(span.link, frame.reader_links.size());
        if (inserted)
          frame.reader_links.push_back({span.link, span.text, row});
        else if (frame.reader_links[entry->second].line == row)
          frame.reader_links[entry->second].label += span.text;
      }
    }
    frame.find_matches =
      render_internal::FindRenderedText(page.lines, prompt.find_query);
    for (int row = 0; row < frame.reader_content_height; ++row) {
      const auto index = offset + static_cast<std::size_t>(row);
      if (index >= page.lines.size())
        break;
      StyledLine line{{" "}};
      auto content =
        render_internal::HighlightMatches(std::move(page.lines[index]),
          frame.find_matches,
          index,
          prompt.current_match);
      for (auto& span : content)
        if (!prompt.selected_link.empty() &&
            span.link == prompt.selected_link) {
          span.role = StyleRole::kCurrentMatch;
          span.underline = true;
        }
      line.insert(line.end(), content.begin(), content.end());
      Place(frame, 2 + row, std::move(line));
    }
    if (state.document && !state.pending && !state.status.starts_with("Error:"))
      status = fmt::format("{}  {}–{}/{}{}",
        title,
        page.lines.empty() ? 0 : offset + 1,
        std::min(offset + visible, page.lines.size()),
        page.lines.size(),
        offset == frame.reader_scroll_limit ? " (END)" : "");
    if (missing_anchor && !state.pending && !state.status.starts_with("Error:"))
      status =
        "Section #" + state.document_anchor + " is unavailable; opened page";
    if (!prompt.selected_link.empty())
      status = "Link: " + prompt.selected_link + " · Enter opens · Esc returns";
  } else {
    Place(frame,
      0,
      {{"manbo", StyleRole::kBrand, true},
        {"  —  offline documentation", StyleRole::kMuted}});
    Place(frame,
      1,
      {{"Query: ", StyleRole::kMuted},
        {state.query.empty() ? "(all installed pages)" : state.query}});
    const bool wide = width >= 100;
    const int left_width = wide ? std::clamp(width * 35 / 100, 32, 54) : width;
    const int right_width = width - left_width - 2;
    const int body_height = height - 5;
    const auto visible =
      static_cast<std::size_t>(std::max(1, (body_height - 1) / 3));
    const auto selection =
      state.hits.empty() ? 0 : std::min(state.selection, state.hits.size() - 1);
    const auto start = selection >= visible ? selection - visible + 1 : 0;
    std::vector<StyledLine> left(static_cast<std::size_t>(body_height));
    const auto shown =
      std::min(visible, state.hits.size() - std::min(start, state.hits.size()));
    for (std::size_t n = 0; n < shown; ++n) {
      const auto index = start + n;
      const auto& hit = state.hits[index];
      const bool selected = index == selection;
      const auto role = selected ? StyleRole::kSelected : StyleRole::kText;
      left[n * 3] = Fit(
        {{Clip(
            fmt::format(
              "{} {:02}  {}", selected ? ">" : " ", index + 1, PageLabel(hit)),
            left_width),
          role,
          selected}},
        left_width,
        role);
      left[n * 3 + 1] = {{Clip("      " + hit.source, left_width),
        selected ? StyleRole::kSelectedMeta : StyleRole::kMuted}};
    }
    left[shown * 3] = {
      {state.pending ? "Searching offline documents..."
        : state.hits.empty()
          ? "No matching pages."
          : fmt::format("{} of {} · {}",
              start + shown,
              state.hits.size(),
              start + shown >= state.hits.size() ? "end of results"
                                                 : "more below"),
        StyleRole::kMuted}};
    std::vector<StyledLine> right;
    if (wide && !state.hits.empty()) {
      const auto& hit = state.hits[selection];
      right.push_back({{PageLabel(hit), StyleRole::kHeading, true}});
      right.push_back({{hit.source, StyleRole::kMuted}});
      right.push_back({});
      if (state.preview && state.preview->id == hit.id) {
        auto page = LayoutDocument(*state.preview, right_width);
        right.insert(right.end(), page.lines.begin(), page.lines.end());
      } else
        right.push_back(
          {{state.preview_error.empty() ? "Loading the selected page..."
                                        : state.preview_error,
            state.preview_error.empty() ? StyleRole::kMuted
                                        : StyleRole::kError}});
    }
    StyledLine heading{
      {state.pending ? "SEARCHING"
                     : fmt::format("{} RESULTS", state.hits.size()),
        StyleRole::kHeading,
        true}};
    if (wide)
      Join(frame,
        3,
        std::move(heading),
        left_width,
        {{"PAGE PREVIEW", StyleRole::kHeading, true}});
    else
      Place(frame, 3, std::move(heading));
    for (int row = 0; row < body_height; ++row) {
      if (wide)
        Join(frame,
          4 + row,
          std::move(left[static_cast<std::size_t>(row)]),
          left_width,
          static_cast<std::size_t>(row) < right.size()
            ? std::move(right[static_cast<std::size_t>(row)])
            : StyledLine{});
      else
        Place(frame, 4 + row, std::move(left[static_cast<std::size_t>(row)]));
    }
    if (!state.pending)
      status = state.hits.empty() ? state.status
                                  : fmt::format("{} · selected {}/{}",
                                      state.status,
                                      selection + 1,
                                      state.hits.size());
  }

  PromptLine(frame, prompt, std::move(status));
  for (auto& line : frame.lines)
    line = Fit(std::move(line), width);
  return frame;
}

auto
PlainLines(const StyledFrame& frame) -> std::vector<std::string> {
  std::vector<std::string> lines;
  for (const auto& line : frame.lines) {
    std::string text;
    for (const auto& span : line)
      text += SafeText(span.text);
    lines.push_back(std::move(text));
  }
  return lines;
}

auto
RenderLines(const Snapshot& state, int width, int height)
  -> std::vector<std::string> {
  return PlainLines(RenderFrame(state, width, height));
}
}  // namespace manbo
