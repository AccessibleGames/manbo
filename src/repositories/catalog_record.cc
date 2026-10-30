#include "repositories/catalog_record.h"

#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>

namespace manbo::catalog_internal {
bool
Within(const std::filesystem::path& catalog_root,
  const std::filesystem::path& source_path) {
  const auto relative_path = std::filesystem::weakly_canonical(source_path)
                               .lexically_relative(catalog_root);
  return !relative_path.empty() && !relative_path.is_absolute() &&
         std::ranges::none_of(relative_path,
           [](const auto& path_component) { return path_component == ".."; });
}

Result<CatalogRecord>
DecodeRecord(const nlohmann::json& catalog_entry,
  const std::filesystem::path& catalog_root) {
  try {
    CatalogRecord entry;
    entry.metadata.id = catalog_entry.at("id").get<std::string>();
    entry.metadata.title = catalog_entry.at("title").get<std::string>();
    entry.metadata.manual_section = catalog_entry.value("manual_section", "");
    entry.metadata.description = catalog_entry.value("description", "");
    entry.metadata.source_url =
      catalog_entry.at("source_url").get<std::string>();

    auto family =
      ParseSourceFamily(catalog_entry.at("family").get<std::string>());
    if (!family)
      return tl::make_unexpected(family.error());
    entry.metadata.family = *family;

    const auto format = catalog_entry.at("format").get<std::string>();
    if (format == "html")
      entry.metadata.format = SourceFormat::kHtml;
    else if (format == "plain" && (*family == SourceFamily::kLinuxMan ||
                                    *family == SourceFamily::kGeneric))
      entry.metadata.format = SourceFormat::kPlainText;
    else
      return tl::make_unexpected(Error{
        ErrorCode::kInvalidArgument, "Unsupported offline document format"});

    entry.metadata.source =
      std::string(SourceFamilyName(*family)) +
      (format == "html" ? " · offline HTML" : " · offline text");
    const auto edition = catalog_entry.value("edition", "");
    if (edition.size() > 128)
      return tl::make_unexpected(Error{
        ErrorCode::kInvalidArgument, "Offline edition label is too long"});
    if (!edition.empty())
      entry.metadata.source += " · " + edition;

    if (entry.metadata.id.empty() || entry.metadata.id.size() > 256 ||
        entry.metadata.title.empty() || entry.metadata.title.size() > 512 ||
        entry.metadata.description.size() > 4096 ||
        entry.metadata.source_url.size() > 2048)
      return tl::make_unexpected(Error{
        ErrorCode::kInvalidArgument, "Invalid offline document metadata"});

    const auto relative =
      std::filesystem::u8path(catalog_entry.at("file").get<std::string>());
    const auto relative_text = relative.generic_string();
    const bool drive_qualified =
      relative_text.size() >= 2 &&
      std::isalpha(static_cast<unsigned char>(relative_text[0])) &&
      relative_text[1] == ':';
    if (relative.empty() || relative.has_root_path() || drive_qualified ||
        relative_text.find('\\') != std::string::npos ||
        std::ranges::any_of(
          relative, [](const auto& part) { return part == ".."; }))
      return tl::make_unexpected(Error{ErrorCode::kInvalidArgument,
        "Offline source path must remain inside its catalog"});

    entry.file = catalog_root / relative;
    if (!Within(catalog_root, entry.file))
      return tl::make_unexpected(Error{ErrorCode::kInvalidArgument,
        "Offline source path escapes its catalog"});
    return entry;
  } catch (const nlohmann::json::exception& error) {
    return tl::make_unexpected(Error{ErrorCode::kInvalidArgument,
      std::string("Invalid offline metadata: ") + error.what()});
  } catch (const std::exception& error) {
    return tl::make_unexpected(Error{ErrorCode::kBackend, error.what()});
  }
}

}  // namespace manbo::catalog_internal
