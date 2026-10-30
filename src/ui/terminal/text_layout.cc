#include <algorithm>
#include <cctype>

#include "ui/terminal/view.h"
#include "view_internal.h"

namespace manbo {
namespace {
struct Glyph {
  std::size_t bytes;
  int columns;
};
Glyph
ReadGlyph(std::string_view text, std::size_t offset) {
  const auto c = static_cast<unsigned char>(text[offset]);
  const std::size_t length = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
  if (offset + length > text.size())
    return {1, 1};
  char32_t code = length == 1   ? c
                  : length == 2 ? c & 0x1f
                  : length == 3 ? c & 0x0f
                                : c & 0x07;
  for (std::size_t n = 1; n < length; ++n)
    code = (code << 6) | (static_cast<unsigned char>(text[offset + n]) & 0x3f);
  if ((code >= 0x300 && code <= 0x36f) || (code >= 0x1ab0 && code <= 0x1aff) ||
      (code >= 0xfe00 && code <= 0xfe0f) || code == 0x200d)
    return {length, 0};
  const bool wide =
    (code >= 0x1100 && code <= 0x115f) || (code >= 0x2e80 && code <= 0xa4cf) ||
    (code >= 0xac00 && code <= 0xd7a3) || (code >= 0xf900 && code <= 0xfaff) ||
    (code >= 0xfe10 && code <= 0xfe19) || (code >= 0xff01 && code <= 0xff60) ||
    (code >= 0x1f300 && code <= 0x1faff) ||
    (code >= 0x20000 && code <= 0x3fffd);
  return {length, wide ? 2 : 1};
}

std::string
Prefix(std::string_view text, int width) {
  std::size_t end = 0;
  int used = 0;
  while (end < text.size()) {
    const auto glyph = ReadGlyph(text, end);
    if (used + glyph.columns > width)
      break;
    used += glyph.columns;
    end += glyph.bytes;
  }
  return std::string(text.substr(0, end));
}
}  // namespace

std::string
SafeText(std::string_view text) {
  std::string result;
  for (std::size_t i = 0; i < text.size();) {
    const auto first = static_cast<unsigned char>(text[i]);
    if (first < 0x80) {
      result += first >= 0x20 && first != 0x7f ? static_cast<char>(first) : '?';
      ++i;
      continue;
    }
    const int length = first >= 0xc2 && first <= 0xdf   ? 2
                       : first >= 0xe0 && first <= 0xef ? 3
                       : first >= 0xf0 && first <= 0xf4 ? 4
                                                        : 0;
    char32_t code = length == 2   ? first & 0x1f
                    : length == 3 ? first & 0x0f
                                  : first & 0x07;
    bool valid =
      length != 0 && i + static_cast<std::size_t>(length) <= text.size();
    for (int j = 1; valid && j < length; ++j) {
      const auto next =
        static_cast<unsigned char>(text[i + static_cast<std::size_t>(j)]);
      valid = (next & 0xc0) == 0x80;
      code = (code << 6) | (next & 0x3f);
    }
    valid = valid && code <= 0x10ffff && !(code >= 0xd800 && code <= 0xdfff) &&
            (length != 3 || code >= 0x800) && (length != 4 || code >= 0x10000);
    if (!valid) {
      result += '?';
      ++i;
      continue;
    }
    if (code >= 0x80 && code <= 0x9f)
      result += '?';
    else
      result.append(text.substr(i, static_cast<std::size_t>(length)));
    i += static_cast<std::size_t>(length);
  }
  return result;
}

std::size_t
TextColumns(std::string_view text) {
  const auto safe = SafeText(text);
  std::size_t columns = 0;
  for (std::size_t offset = 0; offset < safe.size();) {
    const auto glyph = ReadGlyph(safe, offset);
    columns += static_cast<std::size_t>(glyph.columns);
    offset += glyph.bytes;
  }
  return columns;
}

namespace render_internal {
void
Append(StyledLine& line,
  std::string text,
  StyleRole role,
  bool bold,
  bool italic,
  bool underline,
  std::string link) {
  text = SafeText(text);
  if (text.empty())
    return;
  if (!line.empty() && line.back().role == role && line.back().bold == bold &&
      line.back().italic == italic && line.back().underline == underline &&
      line.back().link == link)
    line.back().text += text;
  else
    line.push_back(
      {std::move(text), role, bold, italic, underline, std::move(link)});
}

std::size_t
Columns(const StyledLine& line) {
  std::size_t columns = 0;
  for (const auto& span : line)
    columns += TextColumns(span.text);
  return columns;
}

std::string
Clip(std::string_view text, int width) {
  if (width <= 0)
    return {};
  auto safe = SafeText(text);
  return TextColumns(safe) <= static_cast<std::size_t>(width)
           ? safe
           : Prefix(safe, width - 1) + "…";
}

StyledLine
Fit(StyledLine line, int width, StyleRole padding) {
  StyledLine result;
  int remaining = std::max(0, width);
  for (const auto& span : line) {
    const auto text = Prefix(SafeText(span.text), remaining);
    const auto used = static_cast<int>(TextColumns(text));
    Append(result,
      text,
      span.role,
      span.bold,
      span.italic,
      span.underline,
      span.link);
    remaining -= used;
    if (remaining == 0)
      break;
  }
  if (remaining > 0)
    Append(
      result, std::string(static_cast<std::size_t>(remaining), ' '), padding);
  return result;
}

namespace {
StyleRole
InlineRole(const Inline& run) {
  return !run.link.empty() ? StyleRole::kLink
         : run.code        ? StyleRole::kCode
         : run.strong      ? StyleRole::kStrong
                           : StyleRole::kText;
}

void
AppendInline(StyledLine& line, std::string text, const Inline& style) {
  Append(line,
    std::move(text),
    InlineRole(style),
    style.strong,
    style.emphasis,
    !style.link.empty() || style.underline,
    style.link);
}
}  // namespace

std::vector<StyledLine>
WrapText(const RichText& text, int width) {
  width = std::max(1, width);
  std::vector<StyledLine> lines;
  StyledLine line;
  StyledLine word;
  bool pending_space = false;
  const auto flush_line = [&] {
    lines.push_back(std::move(line));
    line = {};
  };
  const auto flush_word = [&] {
    if (word.empty())
      return;
    const auto word_width = Columns(word);
    const auto separator_width = pending_space && !line.empty() ? 1u : 0u;
    if (!line.empty() && Columns(line) + separator_width + word_width >
                           static_cast<std::size_t>(width))
      flush_line();
    if (pending_space && !line.empty())
      Append(line, " ");

    for (const auto& span : word) {
      for (std::size_t offset = 0; offset < span.text.size();) {
        const auto glyph = ReadGlyph(span.text, offset);
        if (!line.empty() &&
            Columns(line) + glyph.columns > static_cast<std::size_t>(width))
          flush_line();
        Append(line,
          span.text.substr(offset, glyph.bytes),
          span.role,
          span.bold,
          span.italic,
          span.underline,
          span.link);
        offset += glyph.bytes;
      }
    }
    word.clear();
    pending_space = false;
  };

  for (const auto& run : text) {
    if (run.text == "\n") {
      flush_word();
      flush_line();
      pending_space = false;
      continue;
    }
    for (std::size_t offset = 0; offset < run.text.size();) {
      // Inspect source whitespace before sanitizing control characters.
      if (std::isspace(static_cast<unsigned char>(run.text[offset]))) {
        flush_word();
        pending_space = true;
        ++offset;
        continue;
      }
      const auto glyph = ReadGlyph(run.text, offset);
      AppendInline(word,
        SafeText(std::string_view(run.text).substr(offset, glyph.bytes)),
        run);
      offset += glyph.bytes;
    }
    // A style boundary does not end a word: <i>socket</i>() stays together.
  }
  flush_word();
  if (!line.empty())
    flush_line();
  return lines;
}

std::vector<StyledLine>
WrapPreformatted(const RichText& text, int width) {
  width = std::max(1, width);
  std::vector<StyledLine> lines;
  StyledLine line;
  const auto flush_line = [&] {
    lines.push_back(std::move(line));
    line = {};
  };
  const auto wrap_line = [&] {
    std::string visible;
    for (const auto& span : line)
      visible += span.text;
    const auto last_space = visible.find_last_of(' ');
    if (last_space == std::string::npos ||
        visible.find_first_not_of(' ') >= last_space) {
      flush_line();
      return;
    }

    // Preserve spaces and styles while moving the unfinished word to the next
    // line. Split inside a token only when the token itself is wider than us.
    StyledLine before;
    StyledLine after;
    std::size_t remaining = last_space + 1;
    for (const auto& span : line) {
      const auto kept = std::min(remaining, span.text.size());
      Append(before,
        span.text.substr(0, kept),
        span.role,
        span.bold,
        span.italic,
        span.underline,
        span.link);
      Append(after,
        span.text.substr(kept),
        span.role,
        span.bold,
        span.italic,
        span.underline,
        span.link);
      remaining -= kept;
    }
    lines.push_back(std::move(before));
    line = std::move(after);
  };
  for (const auto& run : text) {
    for (std::size_t offset = 0; offset < run.text.size();) {
      const char character = run.text[offset];
      if (character == '\r') {
        ++offset;
        continue;
      }
      if (character == '\n') {
        flush_line();
        ++offset;
        continue;
      }
      if (character == '\t') {
        const auto spaces = 4 - Columns(line) % 4;
        for (std::size_t count = 0; count < spaces; ++count) {
          if (Columns(line) == static_cast<std::size_t>(width))
            wrap_line();
          AppendInline(line, " ", run);
        }
        ++offset;
        continue;
      }
      const auto glyph = ReadGlyph(run.text, offset);
      const auto safe_glyph =
        SafeText(std::string_view(run.text).substr(offset, glyph.bytes));
      if (!line.empty() && Columns(line) + TextColumns(safe_glyph) >
                             static_cast<std::size_t>(width))
        wrap_line();
      AppendInline(line, safe_glyph, run);
      offset += glyph.bytes;
    }
  }
  if (!line.empty())
    flush_line();
  return lines;
}

}  // namespace render_internal
}  // namespace manbo
