#ifndef MANBO_SRC_DOCUMENTS_ADAPTERS_HTML_ADAPTER_INTERNAL_H_
#define MANBO_SRC_DOCUMENTS_ADAPTERS_HTML_ADAPTER_INTERNAL_H_

// Private source-adapter implementation only. No libxml types in public APIs.
#include <libxml/tree.h>

#include <memory>
#include <stop_token>
#include <string>
#include <string_view>

#include "documents/content.h"

namespace manbo::documents_internal {
using NodePredicate = bool (*)(xmlNode*);
enum class HtmlTreatment {
  kDefault,
  kContainer,
  kCode,
  kPreformatted,
  kTableHeader
};
struct HtmlRules {
  std::string_view name;
  NodePredicate root = nullptr;
  NodePredicate marker = nullptr;
  NodePredicate skip = nullptr;
  NodePredicate break_after = nullptr;
  HtmlTreatment (*treatment)(xmlNode*) = nullptr;
  std::string_view language = {};
  bool require_root = true;
  NodePredicate separate_inline = nullptr;
};
std::string
Name(const xmlNode* node);
std::string
Property(xmlNode* node, const char* name);
std::string
NodeText(xmlNode* node);
bool
HasClass(xmlNode* node, std::string_view token);
bool
HasAncestorClass(xmlNode* node, std::string_view token);
Result<void>
ValidateSource(SourceContent source, std::stop_token stop);
Result<RichDocument>
ParseHtml(SourceContent source, const HtmlRules& rules, std::stop_token stop);
}  // namespace manbo::documents_internal

#endif  // MANBO_SRC_DOCUMENTS_ADAPTERS_HTML_ADAPTER_INTERNAL_H_
