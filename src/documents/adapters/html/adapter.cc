#include <fmt/format.h>
#include <libxml/HTMLparser.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <ranges>
#include <utility>

#include "documents/adapters/html/adapter_internal.h"

namespace manbo::documents_internal {
namespace {
constexpr std::size_t kMaxNodes = 100000;
// cppreference's feature-test macro table already exceeds 300 rows. Source
// size and total node bounds still apply independently to larger tables.
constexpr std::size_t kMaxTableRows = 2048;

bool
Space(unsigned char c) {
  return std::isspace(c) != 0;
}

std::string
NormalizeSpaces(std::string_view text, bool preserve_nonbreaking = false) {
  std::string result;
  for (std::size_t n = 0; n < text.size(); ++n) {
    const bool nbsp = text.substr(n, 2) == "\xc2\xa0";
    if (nbsp && preserve_nonbreaking) {
      result += text.substr(n, 2);
      ++n;
      continue;
    }
    if (Space(static_cast<unsigned char>(text[n])) || nbsp) {
      if (result.empty() || result.back() != ' ')
        result += ' ';
      if (nbsp)
        ++n;
    } else
      result += text[n];
  }
  return result;
}

std::string
Trim(std::string text) {
  while (!text.empty() && Space(static_cast<unsigned char>(text.back())))
    text.pop_back();
  const auto first = text.find_first_not_of(" \t\n\r");
  return first == std::string::npos ? "" : text.substr(first);
}

bool
CommonSkip(std::string_view name) {
  return name == "head" || name == "script" || name == "style" ||
         name == "nav" || name == "header" || name == "footer" ||
         name == "iframe" || name == "object" || name == "form" ||
         name == "noscript" || name == "template" || name == "input" ||
         name == "button" || name == "embed";
}

bool
Structural(std::string_view name) {
  return name == "p" || name == "div" || name == "li" || name == "dt" ||
         name == "dd" || name == "tr" || name == "pre" || name == "table" ||
         name == "dl" || name == "ul" || name == "ol";
}

std::string
SafeLink(std::string link) {
  link = Trim(std::move(link));
  const auto colon = link.find(':');
  if (colon != std::string::npos && !link.starts_with("https://") &&
      !link.starts_with("http://"))
    return {};
  if (link.starts_with("//"))
    return {};
  return link;
}

xmlNode*
Find(xmlNode* node,
  NodePredicate predicate,
  std::stop_token stop,
  std::size_t& visited,
  unsigned depth = 0) {
  if (depth > 128 || stop.stop_requested())
    return nullptr;
  for (; node; node = node->next) {
    if (++visited > kMaxNodes || stop.stop_requested())
      return nullptr;
    if (node->type != XML_ELEMENT_NODE || CommonSkip(Name(node)))
      continue;
    if (predicate(node))
      return node;
    if (auto* found = Find(node->children, predicate, stop, visited, depth + 1))
      return found;
  }
  return nullptr;
}
// Trim only the edges of flowing text. Spaces at inline style boundaries carry
// meaning ("Use <em>this</em> value") and must survive until line wrapping.
void
TrimEdges(RichText& text) {
  while (!text.empty()) {
    auto& first = text.front().text;
    first.erase(0, first.find_first_not_of(" \t\n\r"));
    if (!first.empty())
      break;
    text.erase(text.begin());
  }
  while (!text.empty()) {
    auto& last = text.back().text;
    while (!last.empty() && Space(static_cast<unsigned char>(last.back())))
      last.pop_back();
    if (!last.empty())
      break;
    text.pop_back();
  }
}

bool
BlockElement(std::string_view name) {
  return Structural(name) || name == "blockquote" || name == "section" ||
         name == "article" || name == "main" || name == "body" ||
         name == "details" || name == "summary" || name == "figure" ||
         name == "figcaption" || name == "html" || name == "tbody" ||
         name == "thead" || name == "tfoot" || name == "td" || name == "th" ||
         name == "hr" ||
         (name.size() == 2 && name[0] == 'h' && name[1] >= '1' &&
           name[1] <= '6');
}

class HtmlWalker {
 public:
  HtmlWalker(const HtmlRules& rules, std::stop_token stop)
      : rules_(rules), stop_(stop) {
  }

  Result<RichDocument>
  Walk(xmlNode* root) {
    Blocks(root, 0);
    if (stop_.stop_requested())
      return tl::make_unexpected(
        Error{ErrorCode::kCancelled, "HTML adaptation cancelled"});
    if (limited_)
      return tl::make_unexpected(Error{
        ErrorCode::kInvalidArgument, "HTML structure exceeds adapter limits"});
    return std::move(document_);
  }

 private:
  bool
  Visit(unsigned depth) {
    if (limited_ || stop_.stop_requested())
      return false;
    if (++nodes_ > kMaxNodes || depth > 128) {
      limited_ = true;
      return false;
    }
    return true;
  }

  bool
  Skip(xmlNode* node) const {
    return CommonSkip(Name(node)) || (rules_.skip && rules_.skip(node)) ||
           xmlHasProp(node, BAD_CAST "hidden") ||
           Property(node, "aria-hidden") == "true";
  }

  HtmlTreatment
  Treatment(xmlNode* node) const {
    return rules_.treatment ? rules_.treatment(node) : HtmlTreatment::kDefault;
  }

  void
  CaptureAnchor(xmlNode* node) {
    if (node->type != XML_ELEMENT_NODE)
      return;
    auto name = Property(node, "id");
    if (name.empty() && Name(node) == "a")
      name = Property(node, "name");
    if (!name.empty())
      document_.anchors.push_back(
        {std::move(name), document_.blocks.size(), table_row_});
  }

  void
  Break(RichText& output) {
    // Inter-element indentation must not turn one block boundary into several
    // blank lines. Explicit <br> nodes are handled separately.
    while (!output.empty()) {
      auto& trailing = output.back().text;
      if (trailing == "\n")
        return;
      while (
        !trailing.empty() && Space(static_cast<unsigned char>(trailing.back())))
        trailing.pop_back();
      if (!trailing.empty())
        break;
      output.pop_back();
    }
    if (!output.empty())
      output.push_back({"\n"});
  }

  void
  InlineNode(xmlNode* node,
    RichText& output,
    Inline style,
    unsigned depth,
    bool preserve,
    bool html_code = false) {
    if (!Visit(depth))
      return;
    if (node->type == XML_TEXT_NODE) {
      style.text =
        preserve ? NodeText(node) : NormalizeSpaces(NodeText(node), html_code);
      if (html_code && !preserve &&
          (output.empty() || output.back().text.ends_with('\n')))
        style.text.erase(0, style.text.find_first_not_of(' '));
      if (!style.text.empty())
        output.push_back(std::move(style));
      return;
    }
    if (node->type != XML_ELEMENT_NODE || Skip(node))
      return;

    CaptureAnchor(node);
    const auto name = Name(node);
    const bool separated =
      rules_.separate_inline && rules_.separate_inline(node);
    if (!preserve && (BlockElement(name) || separated)) {
      if (separated)
        output.push_back({" "});
      else
        Break(output);
    }

    style.strong =
      style.strong || name == "b" || name == "strong" || name == "th";
    style.emphasis = style.emphasis || name == "i" || name == "em" ||
                     name == "var" || name == "cite";
    style.underline = style.underline || name == "u";
    style.code = style.code || name == "code" || name == "tt" ||
                 name == "kbd" || name == "samp";
    if (name == "a")
      style.link = SafeLink(Property(node, "href"));

    if (name == "br")
      output.push_back({"\n"});
    else if (name == "img") {
      style.text = Property(node, "alt");
      if (!style.text.empty())
        output.push_back(std::move(style));
    } else {
      for (auto* child = node->children; child; child = child->next)
        InlineNode(child,
          output,
          style,
          depth + 1,
          preserve || name == "pre",
          html_code);
    }

    if (!preserve && (BlockElement(name) || separated ||
                       (rules_.break_after && rules_.break_after(node)))) {
      if (separated)
        output.push_back({" "});
      else
        Break(output);
    }
  }

  RichText
  Text(xmlNode* node,
    unsigned depth,
    bool preserve = false,
    bool html_code = false) {
    RichText text;
    for (auto* child = node->children; child; child = child->next)
      InlineNode(child, text, {}, depth + 1, preserve, html_code);
    if (!preserve)
      TrimEdges(text);
    return text;
  }

  ContentBlock
  NewBlock(xmlNode* node, BlockKind kind, unsigned indent) const {
    ContentBlock block;
    block.kind = kind;
    block.indent = indent;
    block.source_line =
      static_cast<std::size_t>(std::max(0L, xmlGetLineNo(node)));
    block.anchor = Property(node, "id");
    return block;
  }

  void
  Emit(ContentBlock block) {
    if (block.kind != BlockKind::kPreformatted &&
        block.kind != BlockKind::kCode)
      TrimEdges(block.text);
    if (!block.text.empty() || !block.rows.empty() ||
        block.kind == BlockKind::kRule)
      document_.blocks.push_back(std::move(block));
  }

  void
  TableRows(xmlNode* node,
    ContentBlock& block,
    unsigned depth,
    unsigned& next_group,
    unsigned group = 0) {
    for (; node; node = node->next) {
      if (!Visit(depth))
        return;
      if (node->type != XML_ELEMENT_NODE || Skip(node))
        continue;
      const auto name = Name(node);
      if (name == "caption") {
        block.text = Text(node, depth);
        continue;
      }
      if (name != "tr") {
        const bool row_group =
          name == "thead" || name == "tbody" || name == "tfoot";
        const auto child_group = row_group ? ++next_group : group;
        TableRows(node->children, block, depth + 1, next_group, child_group);
        continue;
      }

      TableRow row;
      table_row_ = block.rows.size();
      CaptureAnchor(node);
      row.group = group;
      row.header = Treatment(node) == HtmlTreatment::kTableHeader;
      unsigned column_count = 0;
      for (auto* cell = node->children; cell; cell = cell->next) {
        if (!Visit(depth + 1))
          return;
        if ((Name(cell) != "td" && Name(cell) != "th") || Skip(cell))
          continue;
        CaptureAnchor(cell);
        unsigned span = 1;
        const auto attribute = Property(cell, "colspan");
        if (!attribute.empty()) {
          const auto parsed = std::from_chars(
            attribute.data(), attribute.data() + attribute.size(), span);
          if (parsed.ec != std::errc{} || span == 0 || span > 32)
            span = 1;
        }
        column_count += span;
        if (column_count > 32) {
          limited_ = true;
          return;
        }
        row.cells.push_back(Text(cell, depth + 1));
        row.column_spans.push_back(span);
        row.cell_headers.push_back(row.header || Name(cell) == "th");
        unsigned row_span = 1;
        const auto row_attribute = Property(cell, "rowspan");
        if (!row_attribute.empty()) {
          const auto parsed = std::from_chars(row_attribute.data(),
            row_attribute.data() + row_attribute.size(),
            row_span);
          if (parsed.ec != std::errc{} ||
              parsed.ptr != row_attribute.data() + row_attribute.size())
            row_span = 1;
        }
        row.row_spans.push_back(
          std::min(row_span, static_cast<unsigned>(kMaxTableRows)));
      }
      table_row_.reset();
      row.header = row.header || (!row.cells.empty() &&
                                   std::ranges::all_of(row.cell_headers,
                                     [](bool header) { return header; }));
      if (block.rows.size() == kMaxTableRows) {
        limited_ = true;
        return;
      }
      block.rows.push_back(std::move(row));
    }
  }

  void
  List(xmlNode* node, unsigned depth, unsigned indent) {
    const bool ordered = Name(node) == "ol";
    unsigned item_number = 1;
    const auto start = Property(node, "start");
    if (!start.empty())
      std::from_chars(start.data(), start.data() + start.size(), item_number);
    for (auto* item = node->children; item; item = item->next) {
      if (!Visit(depth + 1))
        return;
      if (Name(item) != "li" || Skip(item))
        continue;
      CaptureAnchor(item);
      const auto marker = ordered ? std::to_string(item_number++) + "." : "•";
      const auto content_indent = indent + (ordered ? marker.size() + 1 : 2);
      const auto first_block = document_.blocks.size();
      Blocks(item->children, depth + 2, static_cast<unsigned>(content_indent));
      if (first_block < document_.blocks.size() &&
          document_.blocks[first_block].kind == BlockKind::kParagraph) {
        auto& paragraph = document_.blocks[first_block];
        paragraph.kind = BlockKind::kListItem;
        paragraph.indent = indent;
        paragraph.marker = marker;
      }
    }
  }

  void
  LayoutRow(xmlNode* node, unsigned depth, unsigned indent) {
    const auto first_block = document_.blocks.size();
    Blocks(node->children, depth + 1, indent);
    // Declaration layout tables place overload/version labels in separate
    // cells. Keep them with the declaration instead of making orphan
    // paragraphs.
    for (auto index = first_block; index < document_.blocks.size(); ++index) {
      auto& declaration = document_.blocks[index];
      if (declaration.kind != BlockKind::kCode)
        continue;
      auto annotation_end = index + 1;
      while (annotation_end < document_.blocks.size() &&
             document_.blocks[annotation_end].kind == BlockKind::kParagraph) {
        if (!declaration.annotation.empty())
          declaration.annotation += ' ';
        declaration.annotation +=
          InlineText(document_.blocks[annotation_end].text);
        ++annotation_end;
      }
      document_.blocks.erase(
        document_.blocks.begin() + static_cast<std::ptrdiff_t>(index + 1),
        document_.blocks.begin() + static_cast<std::ptrdiff_t>(annotation_end));
      const auto removed = annotation_end - index - 1;
      for (auto& anchor : document_.anchors) {
        if (anchor.block >= annotation_end)
          anchor.block -= removed;
        else if (anchor.block > index)
          anchor.block = index;
      }
    }
  }

  void
  Blocks(xmlNode* node, unsigned depth, unsigned indent = 0) {
    ContentBlock paragraph;
    paragraph.indent = indent;
    const auto flush_paragraph = [&] {
      Emit(std::move(paragraph));
      paragraph = {};
      paragraph.indent = indent;
    };

    for (; node; node = node->next) {
      if (!Visit(depth))
        return;
      if (node->type != XML_ELEMENT_NODE && node->type != XML_TEXT_NODE)
        continue;
      if (node->type == XML_ELEMENT_NODE && Skip(node))
        continue;
      const auto name = Name(node);
      const auto treatment = node->type == XML_ELEMENT_NODE
                               ? Treatment(node)
                               : HtmlTreatment::kDefault;
      if (!BlockElement(name) && treatment == HtmlTreatment::kDefault) {
        // Inline siblings share one paragraph, regardless of DOM nesting.
        // A <code>, <span>, or <a> boundary is never a paragraph boundary.
        if (paragraph.text.empty())
          paragraph.source_line =
            static_cast<std::size_t>(std::max(0L, xmlGetLineNo(node)));
        InlineNode(node, paragraph.text, {}, depth, false);
        continue;
      }

      flush_paragraph();
      CaptureAnchor(node);
      if (treatment == HtmlTreatment::kContainer) {
        Blocks(node->children, depth + 1, indent);
        continue;
      }
      auto block = NewBlock(node, BlockKind::kParagraph, indent);
      if (name.size() == 2 && name[0] == 'h' && name[1] >= '1' &&
          name[1] <= '6') {
        block.kind = BlockKind::kHeading;
        block.level = static_cast<unsigned>(name[1] - '0');
        block.text = Text(node, depth);
        for (auto* child = node->children; child && block.anchor.empty();
          child = child->next)
          block.anchor = Property(child, "id");
      } else if (name == "pre" || treatment == HtmlTreatment::kCode ||
                 treatment == HtmlTreatment::kPreformatted) {
        bool is_code = treatment == HtmlTreatment::kCode;
        if (treatment == HtmlTreatment::kDefault) {
          is_code = Property(node, "class").find("code") != std::string::npos;
          for (auto* child = node->children; child; child = child->next)
            is_code = is_code || Name(child) == "code";
        }
        block.kind = is_code ? BlockKind::kCode : BlockKind::kPreformatted;
        block.text = Text(node, depth, name == "pre", is_code && name != "pre");
        if (is_code)
          block.language =
            std::string(rules_.language.empty() ? "cpp" : rules_.language);
      } else if (name == "table") {
        block.kind = BlockKind::kTable;
        unsigned next_group = 0;
        TableRows(node->children, block, depth + 1, next_group);
      } else if (name == "dt" || name == "summary") {
        block.kind = BlockKind::kDefinitionTerm;
        block.text = Text(node, depth);
        for (auto& run : block.text)
          run.strong = true;
      } else if (name == "dd") {
        Blocks(node->children, depth + 1, indent + 2);
        continue;
      } else if (name == "ul" || name == "ol") {
        List(node, depth, indent);
        continue;
      } else if (name == "blockquote") {
        const auto first_block = document_.blocks.size();
        Blocks(node->children, depth + 1, indent + 2);
        for (auto index = first_block; index < document_.blocks.size();
          ++index) {
          auto& child = document_.blocks[index];
          if (child.kind == BlockKind::kParagraph &&
              child.indent == indent + 2) {
            child.kind = BlockKind::kQuote;
            child.indent = indent;
          }
        }
        continue;
      } else if (name == "tr") {
        LayoutRow(node, depth, indent);
        continue;
      } else if (name == "hr") {
        block.kind = BlockKind::kRule;
      } else {
        Blocks(node->children, depth + 1, indent);
        continue;
      }
      Emit(std::move(block));
    }
    flush_paragraph();
  }

  const HtmlRules& rules_;
  std::stop_token stop_;
  std::size_t nodes_ = 0;
  bool limited_ = false;
  std::optional<std::size_t> table_row_;
  RichDocument document_;
};
}  // namespace

std::string
Name(const xmlNode* node) {
  return node->name ? reinterpret_cast<const char*>(node->name) : "";
}

std::string
Property(xmlNode* node, const char* name) {
  auto* value = xmlGetProp(node, reinterpret_cast<const xmlChar*>(name));
  if (!value)
    return {};
  std::string result(reinterpret_cast<const char*>(value));
  xmlFree(value);
  return result;
}

std::string
NodeText(xmlNode* node) {
  auto* value = xmlNodeGetContent(node);
  if (!value)
    return {};
  std::string result(reinterpret_cast<const char*>(value));
  xmlFree(value);
  return result;
}

bool
HasClass(xmlNode* node, std::string_view token) {
  if (!node || node->type != XML_ELEMENT_NODE)
    return false;
  const auto value = Property(node, "class");
  for (std::size_t n = 0; n < value.size();) {
    while (n < value.size() && Space(static_cast<unsigned char>(value[n])))
      ++n;
    const auto first = n;
    while (n < value.size() && !Space(static_cast<unsigned char>(value[n])))
      ++n;
    if (std::string_view(value).substr(first, n - first) == token)
      return true;
  }
  return false;
}

bool
HasAncestorClass(xmlNode* node, std::string_view token) {
  for (unsigned n = 0; node && n < 128; ++n, node = node->parent)
    if (HasClass(node, token))
      return true;
  return false;
}

Result<void>
ValidateSource(SourceContent source, std::stop_token stop) {
  if (source.bytes.size() > 2 * 1024 * 1024)
    return tl::make_unexpected(Error{
      ErrorCode::kInvalidArgument, "Source exceeds the 2 MiB adapter limit"});
  if (stop.stop_requested())
    return tl::make_unexpected(
      Error{ErrorCode::kCancelled, "Content adaptation cancelled"});
  return {};
}

Result<RichDocument>
ParseHtml(SourceContent source, const HtmlRules& rules, std::stop_token stop) {
  auto valid = ValidateSource(source, stop);
  if (!valid)
    return tl::make_unexpected(valid.error());
  if (source.format != SourceFormat::kHtml)
    return tl::make_unexpected(Error{ErrorCode::kInvalidArgument,
      fmt::format("{} requires an HTML copy; raw roff requires a "
                  "separate validated formatter",
        rules.name)});
  using HtmlDocument = std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)>;
  HtmlDocument parsed(htmlReadMemory(source.bytes.data(),
                        static_cast<int>(source.bytes.size()),
                        nullptr,
                        "UTF-8",
                        HTML_PARSE_NONET | HTML_PARSE_NOERROR |
                          HTML_PARSE_NOWARNING | HTML_PARSE_COMPACT),
    xmlFreeDoc);
  if (!parsed)
    return tl::make_unexpected(
      Error{ErrorCode::kInvalidArgument, "HTML could not be parsed"});
  auto* document_root = xmlDocGetRootElement(parsed.get());
  std::size_t visited = 0;
  auto* root =
    rules.root ? Find(document_root, rules.root, stop, visited) : nullptr;
  if (!root && rules.require_root)
    return tl::make_unexpected(
      Error{stop.stop_requested() ? ErrorCode::kCancelled
                                  : ErrorCode::kInvalidArgument,
        fmt::format("{} document root was not found", rules.name)});
  if (rules.marker) {
    visited = 0;
    if (!Find(root ? root : document_root, rules.marker, stop, visited))
      return tl::make_unexpected(Error{ErrorCode::kInvalidArgument,
        fmt::format("{} source marker was not found", rules.name)});
  }
  return HtmlWalker(rules, stop).Walk(root ? root->children : document_root);
}
}  // namespace manbo::documents_internal
