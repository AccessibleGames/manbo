#include <fmt/format.h>

#include <CLI/CLI.hpp>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <ranges>
#include <string>

#include "library_commands.h"
#include "manbo.h"
#include "repository_setup.h"
#include "ui/terminal/terminal.h"
#include "ui/terminal/view.h"

namespace manbo {
namespace {
using Json = nlohmann::json;

Json
HitJson(const SearchHit& hit) {
  return {{"id", hit.id},
    {"title", hit.title},
    {"manual_section", hit.manual_section},
    {"description", hit.description},
    {"source", hit.source}};
}

Json
DocumentJson(const Document& document) {
  auto result = HitJson({document.id,
    document.title,
    document.manual_section,
    document.description,
    document.source});
  result["body"] = document.body;
  result["family"] = SourceFamilyName(document.family);
  result["source_url"] = document.source_url;
  return result;
}

int
ReportError(const Error& error, bool json = false) {
  if (json)
    std::cerr << Json{{"error",
                        {{"code", static_cast<int>(error.code)},
                          {"message", error.message}}}}
                   .dump()
              << '\n';
  else
    std::cerr << fmt::format("manbo: {}\n", SafeText(error.message));
  return 1;
}

std::vector<std::string>
DocumentLines(const Document& document) {
  if (document.format == SourceFormat::kHtml)
    return RenderDocumentText(document, 100);
  std::vector<std::string> lines;
  for (auto line : document.body | std::views::split('\n')) {
    std::string text;
    for (char character : line)
      text.push_back(character);
    lines.push_back(SafeText(std::move(text)));
  }
  return lines;
}

class DemoEvents final : public IEventSource {
 public:
  explicit DemoEvents(EventChannel& replies) : replies_(replies) {
  }

  Result<std::optional<Event>>
  Next() override {
    if (!controls_.empty()) {
      auto event = std::move(controls_.front());
      controls_.pop_front();
      return std::optional<Event>{std::move(event)};
    }
    auto reply = replies_.Receive();
    if (reply && std::holds_alternative<SearchCompleted>(*reply)) {
      if (std::get<SearchCompleted>(*reply).hits.empty())
        controls_.push_back(Quit{});
    } else if (reply && std::holds_alternative<ReadCompleted>(*reply)) {
      if (std::get<ReadCompleted>(*reply).preview)
        controls_.push_back(OpenSelected{});
      controls_.push_back(Back{});
      controls_.push_back(Quit{});
    } else
      controls_.push_back(Quit{});
    return reply;
  }

 private:
  EventChannel& replies_;
  std::deque<Event> controls_;
};

class TextRenderer final : public IRenderer {
 public:
  Result<void>
  Render(const Snapshot& state) override {
    pending_ = fmt::format("\n[{}] Query: {} | {}\n",
      state.route == Route::kResults ? "results" : "reader",
      SafeText(state.query),
      SafeText(state.status));
    if (state.document) {
      for (const auto& line : DocumentLines(*state.document))
        pending_ += line + '\n';
    } else {
      for (const auto& hit : state.hits)
        pending_ += fmt::format(
          "  {}  {}\n", SafeText(PageLabel(hit)), SafeText(hit.description));
    }
    return {};
  }

  Result<void>
  Flush() override {
    std::cout << pending_ << std::flush;
    pending_.clear();
    if (!std::cout)
      return tl::make_unexpected(Error{ErrorCode::kBackend, "Output failed"});
    return {};
  }

 private:
  std::string pending_;
};

Json
MetricsJson(const SessionMetrics& metrics) {
  return {{"events", metrics.events},
    {"waits", metrics.waits},
    {"renders", metrics.renders},
    {"flushes", metrics.flushes},
    {"requests", metrics.requests}};
}

int
RunInteractive(Event initial,
  std::size_t workers,
  const std::string& stats_path,
  const RepositoryOptions& options) {
  if (std::holds_alternative<Search>(initial) &&
      std::get<Search>(initial).query.empty())
    initial = ShowSearch{};

  auto channel = EventChannel::Create();
  if (!channel)
    return ReportError(channel.error());
  auto repository = OpenAppRepository(options);
  if (!repository)
    return ReportError(repository.error());
  auto jobs = MakeJobService(**repository, **channel, workers);
  if (!jobs)
    return ReportError(jobs.error());
  auto terminal = MakeTerminal(**channel);
  if (!terminal)
    return ReportError(terminal.error());
  Session session(**terminal, **terminal, **jobs);
  auto result = session.RunInitial(std::move(initial));
  const auto terminal_metrics = (*terminal)->metrics();
  terminal->reset();  // Restore console before diagnostics or file output.
  if (!result)
    return ReportError(result.error());
  if (!stats_path.empty()) {
    auto protected_output = ProtectSourceOutput(options, stats_path);
    if (!protected_output)
      return ReportError(protected_output.error());
    auto stats = MetricsJson(*result);
    stats["frame_bytes"] = terminal_metrics.frame_bytes;
    stats["native_waits"] = terminal_metrics.native_waits;
    stats["input_records"] = terminal_metrics.input_records;
    std::ofstream file(stats_path);
    file << stats.dump(2) << '\n';
    if (!file)
      return ReportError(
        {ErrorCode::kBackend, "Could not write statistics file"});
  }
  return 0;
}

int
RunDemo(std::string query, std::size_t workers, RepositoryOptions options) {
  if (options.directory.empty())
    options.samples = true;
  auto channel = EventChannel::Create();
  if (!channel)
    return ReportError(channel.error());
  auto repository = OpenAppRepository(options);
  if (!repository)
    return ReportError(repository.error());
  auto jobs = MakeJobService(**repository, **channel, workers);
  if (!jobs)
    return ReportError(jobs.error());
  DemoEvents events(**channel);
  TextRenderer renderer;
  Session session(events, renderer, **jobs);
  auto result =
    session.Run(query.empty() ? "std::forward_list" : std::move(query));
  if (!result)
    return ReportError(result.error());
  std::cout << "\nSession statistics: " << MetricsJson(*result).dump() << '\n';
  return 0;
}

int
ExportFrame(std::string query,
  int width,
  int height,
  int selection,
  bool reader,
  const std::string& output,
  const std::string& format,
  const std::string& html_source,
  SourceFamily family,
  const RepositoryOptions& options) {
  if (!html_source.empty() && !options.directory.empty())
    return ReportError(
      {ErrorCode::kInvalidArgument, "Choose either --data or --source-html"});
  if (html_source.empty() && family != SourceFamily::kGeneric)
    return ReportError(
      {ErrorCode::kInvalidArgument, "--source-family requires --source-html"});
  auto protected_output = ProtectSourceOutput(options, output);
  if (!protected_output)
    return ReportError(protected_output.error());
  Snapshot state;
  if (!html_source.empty()) {
    std::error_code identity_error;
    if (std::filesystem::equivalent(html_source, output, identity_error))
      return ReportError({ErrorCode::kInvalidArgument,
        "Frame output must not overwrite its source file"});
    std::ifstream file(html_source, std::ios::binary);
    if (!file)
      return ReportError(
        {ErrorCode::kNotFound, "Could not open local HTML source"});
    std::string bytes(2 * 1024 * 1024 + 1, '\0');
    file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<std::size_t>(file.gcount()));
    auto content = AdaptContent({bytes, SourceFormat::kHtml, "cpp", family});
    if (!content)
      return ReportError(content.error());
    Document document{"local/html",
      "Local HTML document",
      "",
      "",
      std::move(bytes),
      std::filesystem::path(html_source).filename().string(),
      SourceFormat::kHtml,
      *content};
    document.family = family;
    document.source_file = html_source;
    document.title = std::filesystem::path(html_source).filename().string();
    if (family == SourceFamily::kGeneric)
      for (const auto& block : document.content->blocks) {
        if (block.kind == BlockKind::kHeading) {
          document.title = InlineText(block.text);
          break;
        }
      }
    state.hits = {{document.id, document.title, "", "", document.source}};
    state.preview = document;
    state.document = reader ? std::optional<Document>{document} : std::nullopt;
    state.route = reader ? Route::kReader : Route::kResults;
    state.status = "Local source · semantic HTML adapter";
  } else {
    auto repository = OpenAppRepository(options);
    if (!repository)
      return ReportError(repository.error());
    Browser browser;
    query = query.empty() ? "std::forward_list" : std::move(query);
    auto hits = (*repository)->Search(query, {});
    if (!hits)
      return ReportError(hits.error());
    browser.Dispatch(Search{query});
    browser.Dispatch(SearchCompleted{browser.state().generation, *hits});
    if (!hits->empty()) {
      browser.Dispatch(Select{selection});
      auto document =
        (*repository)
          ->Read(browser.state().hits.at(browser.state().selection).id, {});
      if (!document)
        return ReportError(document.error());
      browser.Dispatch(
        ReadCompleted{browser.state().generation, std::move(*document), true});
      if (reader)
        browser.Dispatch(OpenSelected{});
    }
    state = browser.state();
  }
  const auto frame = RenderFrame(state, width, height);
  std::string bytes;
  if (format == "html")
    bytes = SerializeHtml(frame);
  else if (format == "ansi")
    bytes = SerializeAnsi(frame);
  else
    for (const auto& line : PlainLines(frame))
      bytes += line + '\n';
  const auto* original = state.document  ? &*state.document
                         : state.preview ? &*state.preview
                                         : nullptr;
  std::error_code same_file_error;
  if (original && !original->source_file.empty() &&
      std::filesystem::equivalent(
        original->source_file, output, same_file_error))
    return ReportError({ErrorCode::kInvalidArgument,
      "Frame output must not overwrite its source file"});
  const auto path = std::filesystem::path(output);
  std::error_code error;
  if (!path.parent_path().empty())
    std::filesystem::create_directories(path.parent_path(), error);
  if (error)
    return ReportError({ErrorCode::kBackend, error.message()});
  std::ofstream file(path, std::ios::binary);
  file << bytes;
  file.flush();
  if (!file)
    return ReportError({ErrorCode::kBackend, "Frame export failed"});
  std::cout << "Exported rendered frame: " << output << '\n';
  return 0;
}

int
RunCommand(int argc, char** argv) {
  CLI::App app{
    "Manbo: local documentation browser (default: tui); manbo manbo opens its "
    "command "
    "manual"};
  app.set_version_flag("--version", "manbo 0.2.0-prototype");
  std::string query;
  std::string page;
  std::string stats_path;
  RepositoryOptions options;
  options.executable = argv[0];
  LibraryCommands library_commands(app, options.directory);
  app.add_option(
    "--data", options.directory, "Read this offline catalog directory");
  app.add_flag("--samples",
    options.samples,
    "Use the self-authored sample corpus explicitly");
  std::vector<std::string> root_arguments;
  bool json = false;
  std::size_t workers = 1;
  app.add_option("query",
    root_arguments,
    "Query or man [section] name (TUI only with a real console)");
  app.add_flag("--json", json, "Machine-readable search/read results");
  app.add_option(
    "--stats", stats_path, "Write final TUI runtime counters to JSON");
  app
    .add_option(
      "--workers", workers, "0=inline sample backend; 1..16=fixed worker pool")
    ->check(CLI::Range(0, 16));
  auto* search =
    app.add_subcommand("search", "Search the installed offline pages");
  search->add_option(
    "query", query, "Name, symbol, words, or fuzzy title query");
  search->add_flag("--json", json);
  auto* read = app.add_subcommand(
    "read", "Read an offline ID or unambiguous name(section)");
  read->add_option("page", page)->required();
  read->add_flag("--json", json);
  auto* demo = app.add_subcommand(
    "demo", "Run search -> select -> read -> back without a terminal");
  demo->add_option("query", query);
  demo->add_option("--workers", workers)->check(CLI::Range(0, 16));
  auto* tui =
    app.add_subcommand("tui", "Open the native blocking terminal interface");
  tui->add_option("query", query);
  tui->add_option("--workers", workers)->check(CLI::Range(0, 16));
  tui->add_option(
    "--stats", stats_path, "Write final runtime counters to this JSON file");
  int snapshot_width = 132;
  int snapshot_height = 42;
  int snapshot_selection = 0;
  bool snapshot_reader = false;
  std::string snapshot_output;
  std::string snapshot_format = "html";
  std::string snapshot_source;
  std::string snapshot_family = "generic";
  auto* snapshot = app.add_subcommand(
    "snapshot", "Export the actual styled frame for visual review");
  snapshot->add_option("query", query);
  snapshot->add_option("--width", snapshot_width)->check(CLI::Range(40, 240));
  snapshot->add_option("--height", snapshot_height)->check(CLI::Range(10, 100));
  snapshot->add_option("--select", snapshot_selection)
    ->check(CLI::Range(0, 1000));
  snapshot->add_flag("--reader", snapshot_reader);
  snapshot->add_option("--output", snapshot_output)->required();
  snapshot->add_option("--format", snapshot_format)
    ->check(CLI::IsMember({"html", "ansi", "plain"}));
  snapshot->add_option("--source-html",
    snapshot_source,
    "Interpret a local HTML fixture without editing it");
  snapshot
    ->add_option(
      "--source-family", snapshot_family, "Adapter for --source-html")
    ->check(
      CLI::IsMember({"generic", "cppreference", "posix", "gnu", "linux-man"}));
  for (auto* command : {search, read, demo, tui, snapshot})
    command->add_option("--data",
      options.directory,
      "Use this read-only offline catalog directory");
  for (auto* command : {search, read, demo, tui, snapshot})
    command->add_flag("--samples",
      options.samples,
      "Use the self-authored sample corpus explicitly");
  CLI11_PARSE(app, argc, argv);
  if (library_commands.selected() && (options.samples || json))
    return ReportError({ErrorCode::kInvalidArgument,
      "--samples and --json apply to browsing/search/read, not library "
      "maintenance"});
  if (auto result = library_commands.Run()) {
    if (!*result)
      return ReportError(result->error());
    std::cout << **result;
    return std::cout ? 0 : ReportError({ErrorCode::kBackend, "Output failed"});
  }
  for (const auto& argument : root_arguments) {
    if (!query.empty())
      query += ' ';
    query += argument;
  }
  const auto family = ParseSourceFamily(snapshot_family);
  if (!family)
    return ReportError(family.error());
  if (*snapshot)
    return ExportFrame(std::move(query),
      snapshot_width,
      snapshot_height,
      snapshot_selection,
      snapshot_reader,
      snapshot_output,
      snapshot_format,
      snapshot_source,
      *family,
      options);
  if (*demo)
    return RunDemo(std::move(query), workers, options);
  auto initial = (*search || *read) ? Result<Event>{Search{query}}
                                    : ParseStartupQuery(query);
  if (!initial)
    return ReportError(initial.error(), json);
  if (*tui || (!*search && !*read && !json)) {
    if (workers == 0 && !options.samples && !OfflineDirectory(options).empty())
      return ReportError({ErrorCode::kInvalidArgument,
        "Real offline I/O requires --workers 1..16; zero is only for bounded "
        "samples"});
    return RunInteractive(std::move(*initial), workers, stats_path, options);
  }
  auto repository = OpenAppRepository(options);
  if (!repository)
    return ReportError(repository.error(), json);
  const bool direct = std::holds_alternative<OpenPage>(*initial);
  if (direct)
    page = std::get<OpenPage>(*initial).reference;
  else
    query = std::get<Search>(*initial).query;
  if (*read || direct) {
    auto document = (*repository)->Read(page, {});
    if (!document)
      return ReportError(document.error(), json);
    if (json)
      std::cout << DocumentJson(*document).dump(2) << '\n';
    else {
      std::cout << SafeText(PageLabel({document->id,
                     document->title,
                     document->manual_section,
                     document->description,
                     document->source}))
                << '\n';
      for (const auto& line : DocumentLines(*document))
        std::cout << line << '\n';
    }
  } else {
    auto hits = (*repository)->Search(query, {});
    if (!hits)
      return ReportError(hits.error(), json);
    if (json) {
      auto output = Json::array();
      for (const auto& hit : *hits)
        output.push_back(HitJson(hit));
      std::cout << output.dump(2) << '\n';
    } else {
      for (const auto& hit : *hits)
        std::cout << fmt::format("{}\t{}\t{}\n",
          SafeText(PageLabel(hit)),
          SafeText(hit.description),
          SafeText(hit.id));
    }
  }
  if (!std::cout)
    return ReportError({ErrorCode::kBackend, "Output failed"}, json);
  return 0;
}
}  // namespace
}  // namespace manbo

auto
main(int argc, char** argv) -> int {
  try {
    return manbo::RunCommand(argc, argv);
  } catch (const std::exception& error) {
    std::cerr << fmt::format("manbo: {}\n", manbo::SafeText(error.what()));
    return 1;
  }
}
