#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <ranges>

#include "view_internal.h"

namespace manbo::render_internal {
namespace {
StyledLine
Highlight(std::string_view code) {
  constexpr std::string_view keywords[]{"auto",
    "bool",
    "char",
    "class",
    "const",
    "constexpr",
    "double",
    "else",
    "float",
    "for",
    "if",
    "include",
    "int",
    "namespace",
    "noexcept",
    "nullptr",
    "return",
    "sizeof",
    "struct",
    "template",
    "typename",
    "using",
    "void",
    "while"};
  StyledLine line;
  for (std::size_t n = 0; n < code.size();) {
    std::size_t end = n + 1;
    auto role = StyleRole::kCode;
    const auto c = static_cast<unsigned char>(code[n]);
    if (code.substr(n).starts_with("//")) {
      Append(line, std::string(code.substr(n)), StyleRole::kComment);
      break;
    }
    if (c == '"' || c == '\'') {
      bool escape = false;
      while (end < code.size()) {
        const auto next = code[end++];
        if (next == c && !escape)
          break;
        escape = next == '\\' && !escape;
      }
      role = StyleRole::kString;
    } else if (std::isdigit(c)) {
      while (end < code.size() &&
             (std::isalnum(static_cast<unsigned char>(code[end])) ||
               code[end] == '.'))
        ++end;
      role = StyleRole::kNumber;
    } else if (c >= 0x80) {
      while (end < code.size() &&
             (static_cast<unsigned char>(code[end]) & 0xc0) == 0x80)
        ++end;
    } else if (std::isalpha(c) || c == '_') {
      while (end < code.size() &&
             (std::isalnum(static_cast<unsigned char>(code[end])) ||
               code[end] == '_'))
        ++end;
      if (std::ranges::find(keywords, code.substr(n, end - n)) !=
          std::end(keywords))
        role = StyleRole::kKeyword;
    }

    Append(line, std::string(code.substr(n, end - n)), role);
    n = end;
  }
  return line;
}

StyledLine
Highlight(const StyledLine& source) {
  std::string text;
  for (const auto& span : source)
    text += span.text;
  const auto highlighted = Highlight(text);
  StyledLine result;
  std::size_t source_index = 0;
  std::size_t source_offset = 0;
  for (const auto& token : highlighted) {
    std::size_t token_offset = 0;
    while (token_offset < token.text.size() && source_index < source.size()) {
      const auto& original = source[source_index];
      const auto size = std::min(
        token.text.size() - token_offset, original.text.size() - source_offset);
      Append(result,
        token.text.substr(token_offset, size),
        original.link.empty() ? token.role : StyleRole::kLink,
        original.bold,
        original.italic,
        original.underline,
        original.link);
      token_offset += size;
      source_offset += size;
      if (source_offset == original.text.size()) {
        ++source_index;
        source_offset = 0;
      }
    }
  }
  return result;
}

std::string
Rule(int width, std::string_view character = "─") {
  std::string result;
  for (int n = 0; n < width; ++n)
    result += character;
  return result;
}
}  // namespace

DocumentLayout
LayoutDocument(const Document& document, int width) {
  DocumentLayout layout;
  width = std::max(4, width);
  if (!document.content) {
    layout.lines.push_back(
      {{"A content adapter is required for this source.", StyleRole::kNotice}});
    return layout;
  }

  bool previous_list_item = false;
  std::vector<std::size_t> block_lines_begin;
  std::vector<std::vector<std::size_t>> table_row_offsets;
  for (const auto& block : document.content->blocks) {
    block_lines_begin.push_back(layout.lines.size());
    table_row_offsets.emplace_back();
    const int indent = std::min(static_cast<int>(block.indent), width - 4);
    const int content_width = width - indent;
    std::vector<StyledLine> block_lines;
    if (previous_list_item && block.kind != BlockKind::kListItem &&
        block.indent == 0 && !layout.lines.empty() &&
        !layout.lines.back().empty())
      layout.lines.push_back({});
    previous_list_item = block.kind == BlockKind::kListItem;

    if (block.kind == BlockKind::kHeading) {
      if (layout.lines.empty() && block.level == 1 &&
          InlineText(block.text) == document.title) {
        layout.outline.push_back({document.title, 0, block.level});
        if (!block.anchor.empty())
          layout.anchors.push_back({block.anchor, 0});
        continue;
      }
      if (!layout.lines.empty() && !layout.lines.back().empty())
        layout.lines.push_back({});
      layout.outline.push_back(
        {InlineText(block.text), layout.lines.size(), block.level});
      auto heading_lines = WrapText(block.text, content_width - 2);
      for (std::size_t row = 0; row < heading_lines.size(); ++row) {
        StyledLine heading;
        Append(heading,
          row == 0 && block.level <= 2 ? "▎ " : "  ",
          StyleRole::kBrand);
        for (const auto& run : heading_lines[row])
          Append(heading,
            run.text,
            StyleRole::kHeading,
            true,
            run.italic,
            run.underline,
            run.link);
        block_lines.push_back(std::move(heading));
      }
    } else if (block.kind == BlockKind::kCode) {
      const bool bordered = content_width >= 8;
      const auto language = block.language == "cpp"  ? "C++"
                            : block.language == "c"  ? "C"
                            : block.language == "sh" ? "SHELL"
                                                     : "CODE";
      if (bordered) {
        const auto label =
          Clip(" " + std::string(language) +
                 (block.annotation.empty() ? "" : " " + block.annotation) + " ",
            content_width - 2);
        block_lines.push_back(
          {{"╭" + label +
              Rule(content_width - 2 - static_cast<int>(TextColumns(label))) +
              "╮",
            StyleRole::kBorder}});
      }
      auto code_lines =
        WrapPreformatted(block.text, content_width - (bordered ? 4 : 0));
      while (!code_lines.empty() && code_lines.back().empty())
        code_lines.pop_back();
      for (const auto& source_line : code_lines) {
        auto highlighted = Highlight(source_line);
        if (bordered) {
          StyledLine row{{"│ ", StyleRole::kBorder}};
          auto fitted =
            Fit(std::move(highlighted), content_width - 4, StyleRole::kCode);
          row.insert(row.end(), fitted.begin(), fitted.end());
          Append(row, " │", StyleRole::kBorder);
          block_lines.push_back(std::move(row));
        } else
          block_lines.push_back(std::move(highlighted));
      }
      if (bordered)
        block_lines.push_back(
          {{"╰" + Rule(content_width - 2) + "╯", StyleRole::kBorder}});
      block_lines.push_back({});
    } else if (block.kind == BlockKind::kPreformatted) {
      block_lines = WrapPreformatted(block.text, content_width);
      block_lines.push_back({});
    } else if (block.kind == BlockKind::kTable) {
      block_lines =
        LayoutTable(block, content_width, &table_row_offsets.back());
    } else if (block.kind == BlockKind::kRule) {
      block_lines.push_back({{Rule(content_width), StyleRole::kBorder}});
    } else {
      const auto marker = block.kind == BlockKind::kQuote ? "┃ "
                          : block.kind == BlockKind::kListItem
                            ? (block.marker.empty() ? "•" : block.marker) + " "
                            : "";
      const auto marker_width = static_cast<int>(TextColumns(marker));
      auto wrapped =
        WrapText(block.text, std::max(1, content_width - marker_width));
      for (std::size_t row = 0; row < wrapped.size(); ++row) {
        StyledLine line;
        if (!marker.empty())
          Append(line,
            row == 0 || block.kind == BlockKind::kQuote
              ? marker
              : std::string(marker_width, ' '),
            StyleRole::kNotice);
        line.insert(line.end(), wrapped[row].begin(), wrapped[row].end());
        block_lines.push_back(std::move(line));
      }
      if (block.kind != BlockKind::kDefinitionTerm &&
          block.kind != BlockKind::kListItem)
        block_lines.push_back({});
    }

    block_lines_begin.back() = layout.lines.size();
    if (!block.anchor.empty())
      layout.anchors.push_back({block.anchor, layout.lines.size()});
    for (auto& line : block_lines) {
      if (indent && !line.empty())
        line.insert(line.begin(), {std::string(indent, ' ')});
      layout.lines.push_back(std::move(line));
    }
  }
  while (!layout.lines.empty() && layout.lines.back().empty())
    layout.lines.pop_back();
  block_lines_begin.push_back(layout.lines.size());
  for (const auto& anchor : document.content->anchors)
    if (anchor.block < block_lines_begin.size()) {
      auto line = block_lines_begin[anchor.block];
      if (anchor.table_row && anchor.block < table_row_offsets.size() &&
          *anchor.table_row < table_row_offsets[anchor.block].size())
        line += table_row_offsets[anchor.block][*anchor.table_row];
      layout.anchors.push_back({anchor.name, line});
    }
  return layout;
}

}  // namespace manbo::render_internal

namespace manbo {
std::vector<std::string>
RenderDocumentText(const Document& document, int width) {
  auto layout =
    render_internal::LayoutDocument(document, std::clamp(width, 4, 240));
  StyledFrame frame;
  frame.lines = std::move(layout.lines);
  return PlainLines(frame);
}
}  // namespace manbo
