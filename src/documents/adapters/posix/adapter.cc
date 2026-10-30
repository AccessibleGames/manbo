#include "documents/adapters/posix/adapter.h"

#include "documents/adapters/html/adapter_internal.h"

namespace manbo::documents_internal {
namespace {
bool
Root(xmlNode* node) {
  return Name(node) == "body";
}

bool
Marker(xmlNode* node) {
  // Chapter and rationale pages use the edition banner instead of mansect
  // headings. Both are part of the official offline specification.
  return HasClass(node, "mansect") ||
         (Name(node) == "center" &&
           NodeText(node).find("The Open Group Base Specifications") !=
             std::string::npos);
}

bool
Skip(xmlNode* node) {
  return HasClass(node, "NAVHEADER") || HasClass(node, "NAVFOOTER");
}

HtmlTreatment
Treatment(xmlNode* node) {
  // POSIX uses blockquotes for its whole document body, not quotations. The
  // synopsis combines <br>, <tt> and italic parameter names rather than <pre>.
  if (Name(node) == "blockquote")
    return HasClass(node, "synopsis") ? HtmlTreatment::kPreformatted
                                      : HtmlTreatment::kContainer;
  if (Name(node) == "pre")
    return HtmlTreatment::kPreformatted;
  return HtmlTreatment::kDefault;
}
class PosixAdapter final : public IContentAdapter {
 public:
  Result<RichDocument>
  Adapt(SourceContent source, std::stop_token stop) const override {
    HtmlRules rules;
    rules.name = "POSIX";
    rules.root = Root;
    rules.marker = Marker;
    rules.skip = Skip;
    rules.treatment = Treatment;
    rules.language = "c";
    return ParseHtml(source, rules, stop);
  }
};
}  // namespace
std::unique_ptr<IContentAdapter>
MakePosixAdapter() {
  return std::make_unique<PosixAdapter>();
}
}  // namespace manbo::documents_internal
