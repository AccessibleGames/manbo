#include "library_commands.h"

#include <fmt/format.h>

#include <CLI/CLI.hpp>
#include <fstream>

#include "ui/terminal/view.h"

namespace manbo {
LibraryCommands::LibraryCommands(CLI::App& app, std::string& directory)
    : directory_(directory) {
  import_ =
    app.add_subcommand("import", "Import a local documentation collection");
  import_->require_subcommand(1, 1);
  html_ = import_->add_subcommand(
    "html", "Import an extracted HTML tree or one HTML file");
  text_ = import_->add_subcommand(
    "text", "Import UTF-8 text, including already formatted manuals");
  for (auto* command : {html_, text_}) {
    command->add_option("source", options_.source)->required();
    command->add_option("--pages",
      options_.pages,
      "Index this relative subtree while retaining all source/support files");
    command->add_option("--data",
      directory_,
      "Library to create or update (default data/library)");
    command
      ->add_option("--pack", options_.pack, "Stable unique collection name")
      ->required();
    command->add_option(
      "--edition", options_.edition, "Version label (default local)");
    command->add_option(
      "--family", family_, "Content adapter (default generic)");
    command->add_option("--source-url",
      options_.source_url,
      "Optional upstream directory URL; never fetched");
  }
  index_ = app.add_subcommand(
    "index", "Rebuild a local catalog's SQLite metadata index");
  index_->add_option(
    "--data", directory_, "Catalog directory (default data/library)");
  format_ = app.add_subcommand(
    "format", "Render a local HTML file as terminal-readable text");
  format_->add_option("source", format_source_)->required();
  format_->add_option("--family", family_, "Content adapter (default generic)");
  format_->add_option("--width", format_width_)->check(CLI::Range(40, 240));
  format_->add_option(
    "--output", format_output_, "Write a new derived file instead of stdout");
}

std::optional<Result<std::string>>
LibraryCommands::Run() const {
  if (!*import_ && !*index_ && !*format_)
    return std::nullopt;
  const auto directory = directory_.empty()
                           ? std::filesystem::path("data/library")
                           : std::filesystem::path(directory_);
  if (*index_) {
    const auto result = RebuildCatalogIndex(directory);
    if (!result)
      return tl::make_unexpected(result.error());
    return fmt::format("Indexed {}\n", (directory / "catalog.json").string());
  }
  const auto family = ParseSourceFamily(family_);
  if (!family)
    return tl::make_unexpected(family.error());
  if (*import_) {
    auto options = options_;
    options.family = *family;
    options.destination = directory;
    options.format = *text_ ? SourceFormat::kPlainText : SourceFormat::kHtml;
    const auto result = ImportDocuments(options);
    if (!result)
      return tl::make_unexpected(result.error());
    return fmt::format("Imported {} pages; {} total at {}\n",
      result->imported,
      result->total,
      result->library.string());
  }

  if (std::filesystem::file_size(format_source_) > 2 * 1024 * 1024)
    return tl::make_unexpected(
      Error{ErrorCode::kInvalidArgument, "HTML source exceeds 2 MiB"});
  std::ifstream source(format_source_, std::ios::binary);
  if (!source)
    return tl::make_unexpected(
      Error{ErrorCode::kNotFound, "Could not open HTML source"});
  std::string bytes(2 * 1024 * 1024 + 1, '\0');
  source.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  bytes.resize(static_cast<std::size_t>(source.gcount()));
  const auto content = AdaptContent({bytes, SourceFormat::kHtml, "", *family});
  if (!content)
    return tl::make_unexpected(content.error());
  if ((*content)->blocks.empty())
    return tl::make_unexpected(Error{ErrorCode::kInvalidArgument,
      "No visible document content; provide a complete HTML body"});
  Document document{"local/html",
    "",
    "",
    "",
    bytes,
    "Local HTML",
    SourceFormat::kHtml,
    *content};
  std::string output;
  for (const auto& line : RenderDocumentText(document, format_width_))
    output += line + '\n';
  if (format_output_.empty())
    return output;
  // Formatting produces a separate artifact; it never replaces existing files.
  if (std::filesystem::exists(format_output_) ||
      std::filesystem::weakly_canonical(format_source_) ==
        std::filesystem::weakly_canonical(format_output_))
    return tl::make_unexpected(Error{ErrorCode::kInvalidArgument,
      "Format output must be a new file, separate from the source"});
  if (!format_output_.parent_path().empty())
    std::filesystem::create_directories(format_output_.parent_path());
  std::ofstream target(format_output_, std::ios::binary);
  target.write(output.data(), static_cast<std::streamsize>(output.size()));
  target.flush();
  if (!target)
    return tl::make_unexpected(
      Error{ErrorCode::kBackend, "Could not write formatted output"});
  return fmt::format("Formatted {}\n", format_output_.string());
}

bool
LibraryCommands::selected() const {
  return *import_ || *index_ || *format_;
}
}  // namespace manbo
