#include "library/library.h"

#include <fmt/chrono.h>
#include <libxml/HTMLparser.h>
#include <libxml/uri.h>
#include <libxml/xpath.h>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <unordered_set>

#include "repositories/catalog_index.h"
#include "repositories/catalog_record.h"
#include "repositories/catalog_staging.h"

namespace manbo {
namespace {
using Json = nlohmann::json;
constexpr std::uintmax_t kPageLimit = 2 * 1024 * 1024;
constexpr std::uintmax_t kAssetLimit = 64 * 1024 * 1024;
constexpr std::uintmax_t kTreeLimit = 4ULL * 1024 * 1024 * 1024;

std::string
PathText(const std::filesystem::path& path) {
  const auto text = path.generic_u8string();
  return {text.begin(), text.end()};
}

std::string
ReadFile(const std::filesystem::path& path, std::uintmax_t limit) {
  const auto size = std::filesystem::file_size(path);
  if (size > limit)
    throw std::runtime_error("File exceeds its size limit: " + PathText(path));
  std::ifstream stream(path, std::ios::binary);
  std::string bytes(static_cast<std::size_t>(size), '\0');
  if (!stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size())))
    throw std::runtime_error("Could not read " + PathText(path));
  return bytes;
}

std::string
FileHash(const std::filesystem::path& path) {
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(
    EVP_MD_CTX_new(), EVP_MD_CTX_free);
  if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1)
    throw std::runtime_error("Could not initialize SHA256");
  std::ifstream stream(path, std::ios::binary);
  if (!stream)
    throw std::runtime_error("Could not hash " + PathText(path));
  std::array<char, 64 * 1024> buffer;
  std::uintmax_t total_bytes = 0;
  while (stream) {
    stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    total_bytes += static_cast<std::uintmax_t>(stream.gcount());
    if (total_bytes > kAssetLimit)
      throw std::runtime_error("File grew beyond the 64 MiB asset limit");
    if (EVP_DigestUpdate(context.get(),
          buffer.data(),
          static_cast<std::size_t>(stream.gcount())) != 1)
      throw std::runtime_error("SHA256 update failed");
  }
  if (stream.bad())
    throw std::runtime_error("Could not finish hashing " + PathText(path));
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest;
  unsigned size = 0;
  if (EVP_DigestFinal_ex(context.get(), digest.data(), &size) != 1)
    throw std::runtime_error("SHA256 finalization failed");
  constexpr char hex[] = "0123456789abcdef";
  std::string result;
  for (unsigned index = 0; index < size; ++index) {
    result += hex[digest[index] >> 4];
    result += hex[digest[index] & 15];
  }
  return result;
}

std::string
Compact(std::string_view text, std::size_t limit) {
  std::string result;
  bool space = false;
  for (const unsigned char character : text) {
    if (std::isspace(character)) {
      space = !result.empty();
      continue;
    }
    if (space)
      result += ' ';
    result += static_cast<char>(character);
    space = false;
  }
  if (result.size() > limit) {
    auto end = limit;
    while (end && (static_cast<unsigned char>(result[end]) & 0xc0) == 0x80)
      --end;
    result.resize(end);
  }
  return result;
}

std::string
HtmlTitle(const std::string& bytes) {
  using Html = std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)>;
  Html html(htmlReadMemory(bytes.data(),
              static_cast<int>(bytes.size()),
              nullptr,
              "UTF-8",
              HTML_PARSE_NONET | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING),
    xmlFreeDoc);
  if (!html)
    throw std::runtime_error("HTML could not be parsed");
  using Context =
    std::unique_ptr<xmlXPathContext, decltype(&xmlXPathFreeContext)>;
  using Value = std::unique_ptr<xmlXPathObject, decltype(&xmlXPathFreeObject)>;
  Context context(xmlXPathNewContext(html.get()), xmlXPathFreeContext);
  if (!context)
    throw std::runtime_error("Could not inspect HTML title");
  Value value(xmlXPathEvalExpression(BAD_CAST "string(//title)", context.get()),
    xmlXPathFreeObject);
  return value && value->stringval
           ? Compact(reinterpret_cast<const char*>(value->stringval), 512)
           : "";
}

std::string
SourceUrl(std::string_view base, const std::filesystem::path& relative) {
  if (base.empty())
    return {};
  using Text = std::unique_ptr<xmlChar, decltype(xmlFree)>;
  const auto path = PathText(relative);
  Text escaped(xmlURIEscapeStr(BAD_CAST path.c_str(), BAD_CAST "/"), xmlFree);
  if (!escaped)
    throw std::runtime_error("Could not encode source URL");
  return std::string(base) + reinterpret_cast<const char*>(escaped.get());
}

bool
IsPage(const std::filesystem::path& relative, const ImportOptions& options) {
  auto extension = relative.extension().string();
  std::ranges::transform(extension, extension.begin(), [](unsigned char value) {
    return static_cast<char>(std::tolower(value));
  });
  if (options.format == SourceFormat::kPlainText)
    return extension == ".txt";
  if (extension != ".html" && extension != ".htm")
    return false;
  if (options.family != SourceFamily::kPosix)
    return true;
  const auto directory = relative.begin()->string();
  return (directory == "basedefs" || directory == "functions" ||
           directory == "utilities" || directory == "xrat" ||
           directory == "frontmatter") &&
         relative.stem() != "contents" && relative.stem() != "toc";
}

Json
PageRecord(const std::filesystem::path& original,
  const std::filesystem::path& relative,
  const std::filesystem::path& page_relative,
  const std::filesystem::path& staged,
  const ImportOptions& options) {
  const auto bytes = ReadFile(staged, kPageLimit);
  // Original bytes are exposed by the JSON CLI, so require valid UTF-8 rather
  // than silently transcoding or replacing the user's source.
  static_cast<void>(Json(bytes).dump());
  auto content = AdaptContent({bytes, options.format, "", options.family});
  if (!content)
    throw std::runtime_error(
      PathText(relative) + ": " + content.error().message);
  if ((*content)->blocks.empty())
    throw std::runtime_error(PathText(relative) +
                             ": no visible document content; provide a "
                             "complete HTML body or formatted text");
  auto title = options.format == SourceFormat::kHtml ? HtmlTitle(bytes) : "";
  constexpr std::string_view suffix = " - cppreference.com";
  if (options.family == SourceFamily::kCppReference && title.ends_with(suffix))
    title.resize(title.size() - suffix.size());
  if (title.empty())
    title = PathText(relative.stem());
  std::string section;
  if (options.family == SourceFamily::kPosix &&
      bytes.find("mansect") != bytes.npos) {
    const auto first = page_relative.begin()->string();
    section = first == "functions"   ? "3p"
              : first == "utilities" ? "1p"
              : first == "basedefs"  ? "0p"
                                     : "";
  }
  if (options.format == SourceFormat::kPlainText &&
      options.family == SourceFamily::kLinuxMan) {
    const auto stem = relative.stem();
    section = stem.extension().string();
    if (section.size() < 2 || section.size() > 9 ||
        !std::isdigit(static_cast<unsigned char>(section[1])) ||
        !std::ranges::all_of(std::string_view(section).substr(1),
          [](unsigned char c) { return std::isalnum(c) != 0; }))
      throw std::runtime_error(
        "Formatted manuals need filenames such as socket.2.txt");
    section.erase(0, 1);
    title = PathText(stem.stem());
  }
  std::string description;
  for (const auto& block : (*content)->blocks) {
    if (block.kind == BlockKind::kParagraph ||
        block.kind == BlockKind::kQuote) {
      description = Compact(InlineText(block.text), 512);
      if (!description.empty())
        break;
    }
  }
  auto identity_path = page_relative;
  identity_path.replace_extension();
  return {{"id", options.pack + '/' + PathText(identity_path)},
    {"title", title},
    {"manual_section", section},
    {"family", SourceFamilyName(options.family)},
    {"format", options.format == SourceFormat::kHtml ? "html" : "plain"},
    {"description", description},
    {"edition", options.edition},
    {"pack", options.pack},
    {"file", "sources/" + options.pack + '/' + PathText(relative)},
    {"source_url", SourceUrl(options.source_url, page_relative)},
    {"imported_from", PathText(original)},
    {"sha256", FileHash(staged)},
    {"bytes", bytes.size()}};
}

void
WriteJson(const std::filesystem::path& path, const Json& value) {
  const auto text = value.dump(2) + '\n';
  if (text.size() > 64 * 1024 * 1024)
    throw std::runtime_error("Catalog exceeds 64 MiB");
  std::ofstream output(path, std::ios::binary);
  output.write(text.data(), static_cast<std::streamsize>(text.size()));
  output.flush();
  if (!output)
    throw std::runtime_error("Could not write " + PathText(path));
}
}  // namespace

Result<void>
RebuildCatalogIndex(
  const std::filesystem::path& library, std::stop_token stop) {
  return CatalogIndex::Rebuild(library, stop);
}

Result<ImportSummary>
ImportDocuments(const ImportOptions& requested, std::stop_token stop) {
  try {
    auto options = requested;
    if (options.pack.empty() || options.pack.size() > 64 ||
        !std::isalnum(static_cast<unsigned char>(options.pack.front())) ||
        !std::ranges::all_of(options.pack,
          [](unsigned char character) {
            return (character >= 'a' && character <= 'z') ||
                   (character >= '0' && character <= '9') || character == '-';
          }) ||
        options.edition.size() > 128)
      throw std::runtime_error(
        "Use a lowercase pack name of 1..64 characters and an edition of at "
        "most 128 bytes");
    if (options.format == SourceFormat::kRoff ||
        (options.format == SourceFormat::kPlainText &&
          options.family != SourceFamily::kGeneric &&
          options.family != SourceFamily::kLinuxMan))
      throw std::runtime_error(
        "Import HTML or already formatted generic/Linux-man text");
    if (!options.source_url.empty()) {
      using Uri = std::unique_ptr<xmlURI, decltype(&xmlFreeURI)>;
      Uri uri(xmlParseURI(options.source_url.c_str()), xmlFreeURI);
      if (!uri || !uri->scheme || !uri->server || uri->user || uri->query ||
          uri->fragment ||
          (std::string_view(uri->scheme) != "https" &&
            std::string_view(uri->scheme) != "http"))
        throw std::runtime_error(
          "--source-url must be an HTTP(S) directory URL");
      if (!options.source_url.ends_with('/'))
        options.source_url += '/';
    }
    if (std::filesystem::is_symlink(
          std::filesystem::symlink_status(options.source)))
      throw std::runtime_error(
        "Import a regular directory or file, not a symlink");
    const auto source = std::filesystem::canonical(options.source);
    const auto destination =
      std::filesystem::weakly_canonical(options.destination);
    if (catalog_internal::Within(source, destination) ||
        catalog_internal::Within(destination, source))
      throw std::runtime_error(
        "Source and destination must not contain each other");
    std::filesystem::create_directories(destination);
    Json catalog{{"schema_version", 1}, {"documents", Json::array()}};
    if (std::filesystem::exists(destination / "catalog.json"))
      catalog =
        Json::parse(ReadFile(destination / "catalog.json", 64 * 1024 * 1024));
    if (catalog.at("schema_version") != 1 ||
        !catalog.at("documents").is_array())
      throw std::runtime_error("Unsupported catalog schema");

    std::vector<std::filesystem::path> files;
    const auto source_root =
      std::filesystem::is_directory(source) ? source : source.parent_path();
    if (options.pages.has_root_path() ||
        std::ranges::any_of(
          options.pages, [](const auto& part) { return part == ".."; }))
      throw std::runtime_error(
        "--pages must name a subtree inside the source directory");
    const auto page_root =
      options.pages.empty()
        ? source_root
        : std::filesystem::canonical(source_root / options.pages);
    if (!catalog_internal::Within(source_root, page_root) ||
        !std::filesystem::is_directory(page_root))
      throw std::runtime_error(
        "--pages must name a directory inside the source tree");
    if (std::filesystem::is_regular_file(source))
      files.push_back(source);
    else
      for (const auto& entry :
        std::filesystem::recursive_directory_iterator(source)) {
        if (stop.stop_requested())
          throw std::runtime_error("Import cancelled");
        const auto status = entry.symlink_status();
        if (std::filesystem::is_symlink(status) ||
            (!std::filesystem::is_regular_file(status) &&
              !std::filesystem::is_directory(status)))
          throw std::runtime_error(
            "Import trees must contain only regular files/directories");
        if (entry.is_regular_file())
          files.push_back(entry.path());
        if (files.size() > 100000)
          throw std::runtime_error("Source tree exceeds 100,000 files");
      }
    std::ranges::sort(files);
    catalog_internal::StagingDirectory staging(destination);
    const auto staged_sources = staging.path() / "sources";
    std::filesystem::create_directory(staged_sources);
    Json documents = Json::array();
    Json hashes = Json::object();
    std::uintmax_t total_bytes = 0;
    for (const auto& file : files) {
      if (stop.stop_requested())
        throw std::runtime_error("Import cancelled");
      const auto relative = file.lexically_relative(source_root);
      const auto size = std::filesystem::file_size(file);
      if (size > kAssetLimit || total_bytes + size > kTreeLimit)
        throw std::runtime_error(
          "Import exceeds 64 MiB per asset or 4 GiB per tree");
      total_bytes += size;
      const auto copy = staged_sources / relative;
      std::filesystem::create_directories(copy.parent_path());
      std::filesystem::copy_file(file, copy);
      if (std::filesystem::file_size(copy) != size ||
          FileHash(file) != FileHash(copy))
        throw std::runtime_error(
          "Source changed during import: " + PathText(relative));
      hashes[PathText(relative)] = FileHash(copy);
      const auto page_relative = file.lexically_relative(page_root);
      if (!page_relative.empty() &&
          std::ranges::none_of(
            page_relative, [](const auto& part) { return part == ".."; }) &&
          IsPage(page_relative, options))
        documents.push_back(
          PageRecord(file, relative, page_relative, copy, options));
    }
    if (documents.empty())
      throw std::runtime_error(
        "No supported pages found in the selected source tree");

    std::unordered_set<std::string> identities;
    Json retained = Json::array();
    for (const auto& record : catalog.at("documents")) {
      if (record.value("pack", "") == options.pack)
        continue;
      auto checked = catalog_internal::DecodeRecord(record, destination);
      if (!checked)
        throw std::runtime_error(checked.error().message);
      if (!identities.insert(record.at("id").get<std::string>()).second)
        throw std::runtime_error("Duplicate offline document ID");
      retained.push_back(record);
    }
    for (const auto& record : documents) {
      if (!identities.insert(record.at("id").get<std::string>()).second)
        throw std::runtime_error(
          "Pack ID conflicts with another catalog entry");
      auto checked = catalog_internal::DecodeRecord(record, destination);
      if (!checked)
        throw std::runtime_error(checked.error().message);
      retained.push_back(record);
    }
    if (retained.size() > 100000)
      throw std::runtime_error("Catalog exceeds 100,000 pages");
    catalog["documents"] = std::move(retained);
    const auto published_sources = destination / "sources" / options.pack;
    if (std::filesystem::exists(published_sources)) {
      if (std::filesystem::is_symlink(published_sources))
        throw std::runtime_error("Pack sources must not be symlinks");
      std::size_t count = 0;
      for (const auto& entry :
        std::filesystem::recursive_directory_iterator(published_sources)) {
        if (entry.is_symlink())
          throw std::runtime_error("Pack sources must not contain symlinks");
        if (!entry.is_regular_file())
          continue;
        const auto relative =
          PathText(entry.path().lexically_relative(published_sources));
        if (!hashes.contains(relative) ||
            hashes.at(relative) != FileHash(entry.path()))
          throw std::runtime_error(
            "Pack sources changed; choose a new --pack name for this edition");
        ++count;
      }
      if (count != hashes.size())
        throw std::runtime_error(
          "Pack contents changed; choose a new --pack name for this edition");
    }
    WriteJson(staging.path() / "catalog.json", catalog);
    if (stop.stop_requested())
      throw std::runtime_error("Import cancelled");
    std::filesystem::create_directories(destination / "sources");
    if (!std::filesystem::exists(published_sources))
      std::filesystem::rename(staged_sources, published_sources);
    WriteJson(staging.path() / "manifest.json",
      {{"pack", options.pack},
        {"family", SourceFamilyName(options.family)},
        {"edition", options.edition},
        {"source_directory", PathText(source)},
        {"pages", PathText(options.pages)},
        {"source_url", options.source_url},
        {"files", hashes},
        {"imported_utc",
          fmt::format("{:%FT%TZ}",
            fmt::gmtime(std::chrono::system_clock::to_time_t(
              std::chrono::system_clock::now())))}});
    std::filesystem::create_directories(destination / "manifests");
    std::filesystem::rename(staging.path() / "manifest.json",
      destination / "manifests" / (options.pack + ".json"));
    std::filesystem::rename(
      staging.path() / "catalog.json", destination / "catalog.json");
    auto index = RebuildCatalogIndex(destination, stop);
    if (!index)
      return tl::make_unexpected(Error{index.error().code,
        "Catalog imported; index rebuild failed: " + index.error().message});
    return ImportSummary{
      documents.size(), catalog.at("documents").size(), destination};
  } catch (const std::exception& error) {
    return tl::make_unexpected(
      Error{stop.stop_requested() ? ErrorCode::kCancelled
                                  : ErrorCode::kInvalidArgument,
        error.what()});
  }
}
}  // namespace manbo
