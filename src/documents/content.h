#ifndef MANBO_CONTENT_H_
#define MANBO_CONTENT_H_

#include <cstddef>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"

namespace manbo {

// Source bytes remain unchanged in Document::body. Adapters interpret, not
// edit.
enum class SourceFormat { kPlainText, kHtml, kRoff };
// Source family and physical format are distinct: Linux man HTML is not roff.
enum class SourceFamily { kGeneric, kCppReference, kPosix, kGnu, kLinuxMan };
std::string_view
SourceFamilyName(SourceFamily family);
Result<SourceFamily>
ParseSourceFamily(std::string_view name);

struct Inline {
  std::string text;
  bool strong = false;
  bool emphasis = false;
  bool code = false;
  std::string link = {};
  bool underline = false;
};
using RichText = std::vector<Inline>;

enum class BlockKind {
  kHeading,
  kParagraph,
  kDefinitionTerm,
  kCode,
  kPreformatted,
  kListItem,
  kQuote,
  kTable,
  kRule
};
struct TableRow {
  std::vector<RichText> cells;
  bool header = false;
  std::vector<unsigned> column_spans = {};  // Per cell; omitted spans are one.
  std::vector<unsigned> row_spans =
    {};                                 // Zero spans the rest of its row group.
  std::vector<bool> cell_headers = {};  // Row headers need not style all cells.
  unsigned group = 0;  // Distinguishes thead/tbody/tfoot for vertical spans.
};
struct ContentBlock {
  BlockKind kind = BlockKind::kParagraph;
  RichText text = {};  // Tables use this for their caption.
  unsigned level = 2;
  std::string language = {};
  std::string anchor = {};
  std::vector<TableRow> rows = {};
  std::size_t source_line = 0;  // 1-based, or zero when unavailable.
  unsigned indent = 0;          // Extra terminal columns before this block.
  std::string marker = {};  // List bullet or ordinal; empty uses the default.
  std::string annotation = {};  // Declaration overload/version label.
};
struct RichDocument {
  std::vector<ContentBlock> blocks;
  struct Anchor {
    std::string name;
    std::size_t block;
    std::optional<std::size_t> table_row = std::nullopt;
  };
  std::vector<Anchor> anchors = {};
};
struct SourceContent {
  std::string_view bytes;
  SourceFormat format = SourceFormat::kPlainText;
  std::string_view language;
  SourceFamily family = SourceFamily::kGeneric;
};

class IContentAdapter {
 public:
  virtual ~IContentAdapter() = default;
  virtual Result<RichDocument>
  Adapt(SourceContent source, std::stop_token stop = {}) const = 0;
};

std::unique_ptr<IContentAdapter>
MakeContentAdapter(SourceFamily family = SourceFamily::kGeneric);
Result<std::shared_ptr<const RichDocument>>
AdaptContent(SourceContent source, std::stop_token stop = {});
std::string
InlineText(const RichText& text);

}  // namespace manbo

#endif  // MANBO_CONTENT_H_
