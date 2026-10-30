#include "documents/adapters/cppreference/adapter.h"

#include "documents/adapters/html/adapter_internal.h"

namespace manbo::documents_internal {
namespace {
bool
Root(xmlNode* node) {
  return Property(node, "id") == "mw-content-text";
}

bool
Skip(xmlNode* node) {
  return Property(node, "id") == "toc" || HasClass(node, "t-navbar") ||
         HasClass(node, "noprint") || HasClass(node, "editsection") ||
         HasClass(node, "mw-editsection") || HasClass(node, "printfooter") ||
         HasClass(node, "t-example-live-link");
}

bool
BreakAfter(xmlNode* node) {
  return node->parent && HasClass(node->parent, "t-lines");
}

bool
SeparateInline(xmlNode* node) {
  // CSS supplies spacing around version badges in the original page.
  return HasClass(node, "t-mark-rev");
}

HtmlTreatment
Treatment(xmlNode* node) {
  // Older offline books put declarations in the first td, using styled spans
  // and <br> rather than <pre>. Later cells contain overload/version labels.
  if (Name(node) == "td" && node->parent && HasClass(node->parent, "t-dcl")) {
    for (auto* previous = node->prev; previous; previous = previous->prev)
      if (Name(previous) == "td" || Name(previous) == "th")
        return HtmlTreatment::kDefault;
    return HtmlTreatment::kCode;
  }

  // These are CSS layout tables, not data grids. Walk their cells so actual
  // declaration <pre> blocks and version prose keep their own semantics.
  if (HasClass(node, "t-dsc-hitem") || HasClass(node, "t-par-hitem"))
    return HtmlTreatment::kTableHeader;
  if (Name(node) == "table" &&
      (HasClass(node, "t-dcl-begin") || HasClass(node, "t-rev-begin")))
    return HtmlTreatment::kContainer;
  if (Name(node) == "pre")
    return HasClass(node, "source-cpp") ||
               HasAncestorClass(node, "source-cpp") ||
               HasAncestorClass(node, "mw-highlight-lang-cpp") ||
               HasAncestorClass(node, "t-dcl")
             ? HtmlTreatment::kCode
             : HtmlTreatment::kPreformatted;
  return HtmlTreatment::kDefault;
}

class CppReferenceAdapter final : public IContentAdapter {
 public:
  Result<RichDocument>
  Adapt(SourceContent source, std::stop_token stop) const override {
    HtmlRules rules;
    rules.name = "cppreference";
    rules.root = Root;
    rules.skip = Skip;
    rules.break_after = BreakAfter;
    rules.separate_inline = SeparateInline;
    rules.treatment = Treatment;
    rules.language = "cpp";
    return ParseHtml(source, rules, stop);
  }
};
}  // namespace

std::unique_ptr<IContentAdapter>
MakeCppReferenceAdapter() {
  return std::make_unique<CppReferenceAdapter>();
}
}  // namespace manbo::documents_internal
