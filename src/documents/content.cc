#include "documents/content.h"

#include <utility>

#include "documents/adapters/cppreference/adapter.h"
#include "documents/adapters/gnu/adapter.h"
#include "documents/adapters/linux_man/adapter.h"
#include "documents/adapters/plain/adapter.h"
#include "documents/adapters/posix/adapter.h"

namespace manbo {
std::string_view
SourceFamilyName(SourceFamily family) {
  switch (family) {
    case SourceFamily::kCppReference:
      return "cppreference";
    case SourceFamily::kPosix:
      return "posix";
    case SourceFamily::kGnu:
      return "gnu";
    case SourceFamily::kLinuxMan:
      return "linux-man";
    default:
      return "generic";
  }
}

Result<SourceFamily>
ParseSourceFamily(std::string_view name) {
  for (auto family : {SourceFamily::kGeneric,
         SourceFamily::kCppReference,
         SourceFamily::kPosix,
         SourceFamily::kGnu,
         SourceFamily::kLinuxMan})
    if (SourceFamilyName(family) == name)
      return family;
  return tl::make_unexpected(
    Error{ErrorCode::kInvalidArgument, "Unknown documentation source family"});
}

std::unique_ptr<IContentAdapter>
MakeContentAdapter(SourceFamily family) {
  switch (family) {
    case SourceFamily::kCppReference:
      return documents_internal::MakeCppReferenceAdapter();
    case SourceFamily::kPosix:
      return documents_internal::MakePosixAdapter();
    case SourceFamily::kGnu:
      return documents_internal::MakeGnuCommandAdapter();
    case SourceFamily::kLinuxMan:
      return documents_internal::MakeLinuxManAdapter();
    default:
      return documents_internal::MakeGenericAdapter();
  }
}

Result<std::shared_ptr<const RichDocument>>
AdaptContent(SourceContent source, std::stop_token stop) {
  auto document = MakeContentAdapter(source.family)->Adapt(source, stop);
  if (!document)
    return tl::make_unexpected(document.error());
  return std::make_shared<const RichDocument>(std::move(*document));
}

std::string
InlineText(const RichText& text) {
  std::string result;
  for (const auto& run : text)
    result += run.text;
  return result;
}
}  // namespace manbo
