#include "documents/adapters/plain/adapter.h"

#include <algorithm>
#include <cctype>
#include <ranges>
#include <utility>

#include "documents/adapters/html/adapter_internal.h"

namespace manbo::documents_internal {
namespace {
std::string
Trim(std::string_view text) {
  const auto space = [](unsigned char c) { return std::isspace(c) != 0; };
  while (!text.empty() && space(static_cast<unsigned char>(text.front())))
    text.remove_prefix(1);
  while (!text.empty() && space(static_cast<unsigned char>(text.back())))
    text.remove_suffix(1);
  return std::string(text);
}

bool
IsHeading(std::string_view line) {
  return !line.empty() && line.size() <= 64 &&
         std::isupper(static_cast<unsigned char>(line.front())) &&
         std::ranges::all_of(line,
           [](unsigned char c) { return !std::isalpha(c) || std::isupper(c); });
}

RichText
ReferenceText(std::string_view text) {
  RichText result;
  std::size_t offset = 0;
  while (offset < text.size()) {
    const auto start = offset;
    const bool space = std::isspace(static_cast<unsigned char>(text[offset]));
    while (offset < text.size() && (std::isspace(static_cast<unsigned char>(
                                      text[offset])) != 0) == space)
      ++offset;
    const std::string token(text.substr(start, offset - start));
    if (space) {
      result.push_back({token});
      continue;
    }
    std::string target = token;
    while (!target.empty() && (target.back() == ',' || target.back() == '.'))
      target.pop_back();
    const bool reference =
      target.starts_with("std::") ||
      (target.find('(') != std::string::npos && target.ends_with(')'));
    result.push_back({token, false, false, reference, reference ? target : ""});
  }
  return result;
}

Result<RichDocument>
AdaptPlain(SourceContent source, std::stop_token stop) {
  RichDocument document;
  std::string section;
  std::string paragraph;
  std::size_t block_line = 0;
  const bool formatted_man = source.family == SourceFamily::kLinuxMan;
  bool preserve_layout = false;
  const auto code_section = [&] {
    return section == "SYNOPSIS" || section == "EXAMPLES" ||
           (formatted_man && section == "EXAMPLE");
  };

  const auto flush = [&] {
    if (paragraph.empty())
      return;

    ContentBlock block;
    if (code_section())
      block.kind = BlockKind::kCode;
    else if (preserve_layout)
      block.kind = BlockKind::kPreformatted;
    else if (section == "NOTE" || section == "NOTES")
      block.kind = BlockKind::kQuote;
    else
      block.kind = BlockKind::kParagraph;

    if (formatted_man && !code_section() && !preserve_layout)
      std::ranges::replace(paragraph, '\n', ' ');
    block.text =
      section == "SEE ALSO" ? ReferenceText(paragraph) : RichText{{paragraph}};
    block.language = std::string(source.language);
    block.source_line = block_line;
    document.blocks.push_back(std::move(block));
    paragraph.clear();
    preserve_layout = false;
  };

  std::size_t number = 0;
  for (auto part : source.bytes | std::views::split('\n')) {
    if (stop.stop_requested())
      return tl::make_unexpected(
        Error{ErrorCode::kCancelled, "Content adaptation cancelled"});
    ++number;
    std::string line;
    for (char character : part)
      line.push_back(character);
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (IsHeading(line)) {
      flush();
      section = line;
      ContentBlock heading;
      heading.kind = BlockKind::kHeading;
      heading.text = {{line}};
      heading.source_line = number;
      heading.anchor = line;
      document.blocks.push_back(std::move(heading));
    } else if (Trim(line).empty()) {
      flush();
    } else {
      if (paragraph.empty())
        block_line = number;
      const bool code = code_section();
      if (!paragraph.empty())
        paragraph += code || formatted_man ? '\n' : ' ';
      if (formatted_man) {
        // groff's normal body margin is seven columns. Preserve deeper
        // indentation, option lists and tables; reflow ordinary prose only.
        const auto indent = line.find_first_not_of(' ');
        preserve_layout =
          preserve_layout || indent != 7 || line.find("│") != std::string::npos;
        paragraph += line.starts_with("       ") ? line.substr(7) : line;
      } else {
        paragraph +=
          code ? line.starts_with("  ") ? line.substr(2) : line : Trim(line);
      }
    }
  }

  flush();
  return document;
}

bool
GenericRoot(xmlNode* node) {
  return Property(node, "id") == "mw-content-text" || Name(node) == "main" ||
         Name(node) == "article";
}
class GenericAdapter final : public IContentAdapter {
 public:
  auto
  Adapt(SourceContent source, std::stop_token stop) const
    -> Result<RichDocument> override {
    auto valid = ValidateSource(source, stop);
    if (!valid)
      return tl::make_unexpected(valid.error());
    if (source.format == SourceFormat::kPlainText)
      return AdaptPlain(source, stop);
    if (source.format == SourceFormat::kRoff)
      return tl::make_unexpected(Error{ErrorCode::kInvalidArgument,
        "Raw man/mdoc requires a validated formatter adapter; regex "
        "stripping is not supported"});
    HtmlRules rules;
    rules.name = "Generic HTML";
    rules.root = GenericRoot;
    rules.require_root = false;
    return ParseHtml(source, rules, stop);
  }
};
}  // namespace

std::unique_ptr<IContentAdapter>
MakeGenericAdapter() {
  return std::make_unique<GenericAdapter>();
}
}  // namespace manbo::documents_internal
