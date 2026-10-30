#include "documents/adapters/gnu/adapter.h"

#include "documents/adapters/html/adapter_internal.h"

namespace manbo::documents_internal {
namespace {
bool
Root(xmlNode* node) {
  return HasClass(node, "section-level-extent") ||
         HasClass(node, "subsection-level-extent") ||
         HasClass(node, "chapter-level-extent");
}

bool
Skip(xmlNode* node) {
  return HasClass(node, "nav-panel") || HasClass(node, "copiable-link") ||
         HasClass(node, "index-entry-id");
}

HtmlTreatment
Treatment(xmlNode* node) {
  if (Name(node) == "pre")
    return HtmlTreatment::kPreformatted;
  if (Name(node) == "blockquote" && HasClass(node, "indentedblock"))
    return HtmlTreatment::kContainer;
  return HtmlTreatment::kDefault;
}
class GnuCommandAdapter final : public IContentAdapter {
 public:
  Result<RichDocument>
  Adapt(SourceContent source, std::stop_token stop) const override {
    HtmlRules rules;
    rules.name = "GNU Texinfo command manual";
    rules.root = Root;
    rules.skip = Skip;
    rules.treatment = Treatment;
    rules.language = "sh";
    return ParseHtml(source, rules, stop);
  }
};
}  // namespace
std::unique_ptr<IContentAdapter>
MakeGnuCommandAdapter() {
  return std::make_unique<GnuCommandAdapter>();
}
}  // namespace manbo::documents_internal
