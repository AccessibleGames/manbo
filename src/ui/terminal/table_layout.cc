#include <algorithm>
#include <array>
#include <numeric>
#include <optional>

#include "view_internal.h"

namespace manbo::render_internal {
namespace {
struct Cell {
  const RichText* text;
  unsigned column;
  unsigned span;
  bool header;
  bool continued = false;
  std::size_t until_row = 0;
};

struct Row {
  std::vector<Cell> cells;
  bool header;
};

struct Grid {
  std::vector<Row> rows;
  unsigned columns = 0;
};

unsigned
ColumnSpan(const TableRow& row, std::size_t cell) {
  return cell < row.column_spans.size()
           ? std::clamp(row.column_spans[cell], 1u, 32u)
           : 1;
}

std::optional<Grid>
PlaceCells(const ContentBlock& table) {
  Grid grid;
  std::vector<Cell> active;
  unsigned previous_group = 0;
  for (std::size_t row_index = 0; row_index < table.rows.size(); ++row_index) {
    const auto& source = table.rows[row_index];
    if (source.group != previous_group)
      active.clear();
    previous_group = source.group;
    std::erase_if(
      active, [&](const auto& cell) { return cell.until_row <= row_index; });

    Row row{{}, source.header};
    std::array<bool, 32> occupied{};
    for (auto cell : active) {
      cell.continued = true;
      row.cells.push_back(cell);
      for (unsigned offset = 0; offset < cell.span; ++offset)
        occupied[cell.column + offset] = true;
    }

    unsigned column = 0;
    for (std::size_t index = 0; index < source.cells.size(); ++index) {
      const auto span = ColumnSpan(source, index);
      while (column + span <= occupied.size() &&
             std::any_of(occupied.begin() + column,
               occupied.begin() + column + span,
               [](bool used) { return used; }))
        ++column;
      if (column + span > occupied.size())
        return std::nullopt;

      const auto row_span =
        index < source.row_spans.size() ? source.row_spans[index] : 1;
      const auto until =
        row_span == 0 ? table.rows.size() : row_index + row_span;
      Cell cell{&source.cells[index],
        column,
        span,
        index < source.cell_headers.size() ? source.cell_headers[index]
                                           : source.header,
        false,
        until};
      row.cells.push_back(cell);
      if (until > row_index + 1)
        active.push_back(cell);
      for (unsigned offset = 0; offset < span; ++offset)
        occupied[column + offset] = true;
      column += span;
    }

    std::ranges::sort(row.cells, {}, &Cell::column);
    for (const auto& cell : row.cells)
      grid.columns = std::max(grid.columns, cell.column + cell.span);
    grid.rows.push_back(std::move(row));
  }
  return grid;
}

Grid
UnpositionedCells(const ContentBlock& table) {
  // Invalid overlapping spans still expose every source cell in stacked form.
  Grid grid;
  for (const auto& source : table.rows) {
    Row row{{}, source.header};
    unsigned column = 0;
    for (std::size_t index = 0; index < source.cells.size(); ++index) {
      const auto span = ColumnSpan(source, index);
      row.cells.push_back({&source.cells[index], column, span, source.header});
      column += span;
    }
    grid.columns = std::max(grid.columns, column);
    grid.rows.push_back(std::move(row));
  }
  return grid;
}

const RichText&
CellText(const Cell& cell) {
  // Keep long spanning content once. The continuation marker also preserves
  // column context without multiplying source text across thousands of rows.
  static const RichText continuation{{"↑"}};
  return cell.continued ? continuation : *cell.text;
}

int
PreferredWidth(const RichText& text) {
  const auto plain_text = InlineText(text);
  int preferred = 1;
  std::size_t start = 0;
  while (start < plain_text.size()) {
    const auto end = plain_text.find('\n', start);
    const auto line =
      std::string_view(plain_text)
        .substr(start,
          end == std::string::npos ? plain_text.size() - start : end - start);
    preferred = std::max(preferred, static_cast<int>(TextColumns(line)));
    if (end == std::string::npos)
      break;
    start = end + 1;
  }
  return preferred;
}

std::vector<StyledLine>
StackRows(const Grid& grid, int width, std::vector<std::size_t>& row_offsets) {
  std::vector<StyledLine> lines;
  std::vector<RichText> labels;
  std::size_t header_rows = 0;
  while (header_rows < grid.rows.size() && grid.rows[header_rows].header)
    ++header_rows;
  if (header_rows && header_rows < grid.rows.size()) {
    const auto& leaf_headers = grid.rows[header_rows - 1].cells;
    if (leaf_headers.size() == grid.columns &&
        std::ranges::all_of(
          leaf_headers, [](const auto& cell) { return cell.span == 1; })) {
      for (const auto& cell : leaf_headers)
        labels.push_back(*cell.text);
    }
  }

  for (std::size_t row_index = 0; row_index < grid.rows.size(); ++row_index) {
    row_offsets.push_back(lines.size());
    const auto& row = grid.rows[row_index];
    const bool labeled_header = row_index < header_rows && !labels.empty();
    const bool definition = labels.empty() && row.cells.size() == 3 &&
                            grid.columns == 3 &&
                            InlineText(*row.cells[1].text) == "-";
    for (std::size_t index = 0; index < row.cells.size(); ++index) {
      const auto& cell = row.cells[index];
      if (labeled_header && cell.span == 1)
        continue;
      auto text = CellText(cell);
      if (text.empty() || (definition && index == 1))
        continue;
      if (!labeled_header && cell.span == 1 && cell.column < labels.size()) {
        RichText labeled = labels[cell.column];
        for (auto& run : labeled)
          run.strong = true;
        labeled.push_back({": "});
        labeled.insert(labeled.end(), text.begin(), text.end());
        text = std::move(labeled);
      } else if (index == 0 || cell.header) {
        for (auto& run : text)
          run.strong = true;
      }

      const int indent = index > 0 && labels.empty() ? 2 : 0;
      auto wrapped = WrapText(text, std::max(1, width - indent));
      for (auto& line : wrapped) {
        if (indent)
          line.insert(line.begin(), {std::string(indent, ' ')});
        lines.push_back(std::move(line));
      }
    }
    if (!lines.empty() && !lines.back().empty())
      lines.push_back({});
  }
  return lines;
}

std::vector<StyledLine>
LayoutGrid(const Grid& grid, int width, std::vector<std::size_t>& row_offsets) {
  const auto column_count = grid.columns;
  if (column_count == 0)
    return {};

  std::vector<int> preferred(column_count, 1);
  for (const auto& row : grid.rows)
    for (const auto& cell : row.cells)
      if (cell.span == 1)
        preferred[cell.column] =
          std::max(preferred[cell.column], PreferredWidth(CellText(cell)));

  const int available = width - 1 - 3 * static_cast<int>(column_count);
  std::vector<int> widths;
  for (const auto desired : preferred)
    widths.push_back(std::min(desired, 24));
  int used = std::accumulate(widths.begin(), widths.end(), 0);
  if (used > available)
    return StackRows(grid, width, row_offsets);
  while (used < available) {
    std::size_t selected = 0;
    for (std::size_t column = 1; column < column_count; ++column)
      if (preferred[column] - widths[column] >
          preferred[selected] - widths[selected])
        selected = column;
    if (preferred[selected] <= widths[selected])
      break;
    ++widths[selected];
    ++used;
  }
  for (std::size_t column = 0; column < column_count; ++column)
    if (preferred[column] > widths[column] && widths[column] < 24)
      return StackRows(grid, width, row_offsets);

  std::vector<StyledLine> lines;
  for (const auto& row : grid.rows) {
    row_offsets.push_back(lines.size());
    std::vector<std::vector<StyledLine>> cells;
    std::vector<int> cell_widths;
    std::vector<bool> headers;
    unsigned column = 0;
    std::size_t height = 1;
    for (const auto& cell : row.cells) {
      while (column < cell.column) {
        cells.push_back({});
        cell_widths.push_back(widths[column++]);
        headers.push_back(false);
      }
      int cell_width = 3 * static_cast<int>(cell.span - 1);
      for (unsigned offset = 0; offset < cell.span; ++offset)
        cell_width += widths[column++];
      auto wrapped = WrapText(CellText(cell), cell_width);
      height = std::max(height, wrapped.size());
      cells.push_back(std::move(wrapped));
      cell_widths.push_back(cell_width);
      headers.push_back(cell.header);
    }
    while (column < column_count) {
      cells.push_back({});
      cell_widths.push_back(widths[column++]);
      headers.push_back(false);
    }

    for (std::size_t line_index = 0; line_index < height; ++line_index) {
      StyledLine line{{"│", StyleRole::kBorder}};
      for (std::size_t cell = 0; cell < cells.size(); ++cell) {
        Append(line, " ");
        auto content =
          Fit(line_index < cells[cell].size() ? cells[cell][line_index]
                                              : StyledLine{},
            cell_widths[cell]);
        if (headers[cell])
          for (auto& run : content) {
            run.role = StyleRole::kTableHeader;
            run.bold = true;
          }
        line.insert(line.end(), content.begin(), content.end());
        Append(line, " │", StyleRole::kBorder);
      }
      lines.push_back(std::move(line));
    }
    if (row.header) {
      std::string rule;
      for (int column = 0;
        column < used + 1 + 3 * static_cast<int>(column_count);
        ++column)
        rule += "─";
      lines.push_back({{std::move(rule), StyleRole::kBorder}});
    }
  }
  lines.push_back({});
  return lines;
}
}  // namespace

std::vector<StyledLine>
LayoutTable(
  const ContentBlock& table, int width, std::vector<std::size_t>* row_offsets) {
  auto caption = table.text;
  for (auto& run : caption)
    run.strong = true;
  auto lines = WrapText(caption, width);
  if (!caption.empty())
    lines.push_back({});

  auto grid = PlaceCells(table);
  std::vector<std::size_t> offsets;
  auto body = grid ? LayoutGrid(*grid, width, offsets)
                   : StackRows(UnpositionedCells(table), width, offsets);
  if (row_offsets) {
    *row_offsets = std::move(offsets);
    for (auto& offset : *row_offsets)
      offset += lines.size();
  }
  lines.insert(lines.end(),
    std::make_move_iterator(body.begin()),
    std::make_move_iterator(body.end()));
  return lines;
}
}  // namespace manbo::render_internal
