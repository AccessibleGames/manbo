#include "documents/adapters/linux_man/adapter.h"

#include "documents/adapters/html/adapter_internal.h"
#include "documents/adapters/plain/adapter.h"

namespace manbo::documents_internal {
namespace {
bool
Root(xmlNode* node) {
  return Name(node) == "body";
}

bool
Marker(xmlNode* node) {
  return Name(node) == "h1" &&
         NodeText(node).find("Linux manual page") != std::string::npos;
}

bool
Skip(xmlNode* node) {
  return Name(node) == "h1" || HasClass(node, "nav-bar") ||
         HasClass(node, "nav-end") || HasClass(node, "sec-table") ||
         HasClass(node, "top-link") || HasClass(node, "page-top") ||
         HasClass(node, "footer") || HasClass(node, "end-footer");
}

HtmlTreatment
Treatment(xmlNode* node) {
  // man7 has real section headings plus formatted <pre> bodies. Keep original
  // indentation and bold/italic/link runs; do not misclassify prose as C++.
  return Name(node) == "pre" ? HtmlTreatment::kPreformatted
                             : HtmlTreatment::kDefault;
}
class LinuxManAdapter final : public IContentAdapter {
 public:
  Result<RichDocument>
  Adapt(SourceContent source, std::stop_token stop) const override {
    // The importer uses the system formatter for roff/mdoc. Already formatted
    // UTF-8 text is portable; raw macros still require an external formatter.
    if (source.format == SourceFormat::kPlainText)
      return MakeGenericAdapter()->Adapt(source, stop);

    HtmlRules rules;
    rules.name = "Linux man7 HTML";
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
MakeLinuxManAdapter() {
  return std::make_unique<LinuxManAdapter>();
}
}  // namespace manbo::documents_internal
