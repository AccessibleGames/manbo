#include "repositories/catalog_index.h"

#include <sqlite3.h>

#include <chrono>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <unordered_set>

#include "repositories/catalog_record.h"
#include "repositories/catalog_staging.h"

namespace manbo {
namespace {
constexpr int kApplicationId = 0x4d414e42;
constexpr std::string_view kRecordFields =
  "SELECT id,title,manual_section,description,source_url,family,format,"
  "file,edition FROM documents WHERE ";
using Connection = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

void
Check(sqlite3* database, int result) {
  if (result != SQLITE_OK)
    throw std::runtime_error(sqlite3_errmsg(database));
}

Connection
Connect(const std::filesystem::path& file, bool writable = false) {
  const auto utf8_path = file.u8string();
  const std::string filename(utf8_path.begin(), utf8_path.end());
  sqlite3* database = nullptr;
  const auto result = sqlite3_open_v2(filename.c_str(),
    &database,
    (writable ? SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE
              : SQLITE_OPEN_READONLY) |
      SQLITE_OPEN_NOMUTEX,
    nullptr);
  Connection connection(database, sqlite3_close);
  if (result != SQLITE_OK)
    throw std::runtime_error(
      database ? sqlite3_errmsg(database) : sqlite3_errstr(result));
  // Indexed metadata is small even when the reference library is large.
  sqlite3_limit(database, SQLITE_LIMIT_LENGTH, 64 * 1024);
  return connection;
}

std::string
Lower(std::string text) {
  for (auto& character : text)
    if (character >= 'A' && character <= 'Z')
      character += 'a' - 'A';
  return text;
}

Statement
Prepare(sqlite3* database, std::string_view sql) {
  sqlite3_stmt* statement = nullptr;
  Check(database,
    sqlite3_prepare_v2(
      database, sql.data(), static_cast<int>(sql.size()), &statement, nullptr));
  return Statement(statement, sqlite3_finalize);
}

void
Bind(sqlite3_stmt* statement, int index, std::string_view value) {
  Check(sqlite3_db_handle(statement),
    sqlite3_bind_text(statement,
      index,
      value.data(),
      static_cast<int>(value.size()),
      SQLITE_TRANSIENT));
}

bool
Step(sqlite3_stmt* statement) {
  const auto result = sqlite3_step(statement);
  if (result != SQLITE_ROW && result != SQLITE_DONE)
    throw std::runtime_error(sqlite3_errmsg(sqlite3_db_handle(statement)));
  return result == SQLITE_ROW;
}

std::string
Text(sqlite3_stmt* statement, int column) {
  if (sqlite3_column_type(statement, column) != SQLITE_TEXT ||
      sqlite3_column_bytes(statement, column) > 4096)
    throw std::runtime_error("Invalid metadata in catalog index");
  const auto* bytes = sqlite3_column_text(statement, column);
  return {reinterpret_cast<const char*>(bytes),
    static_cast<std::size_t>(sqlite3_column_bytes(statement, column))};
}

void
WatchCancellation(sqlite3* database, std::stop_token& stop) {
  // SQLite invokes this synchronously during statements on this connection.
  // The stop token outlives the local connection in Search/Find.
  sqlite3_progress_handler(
    database,
    1000,
    [](void* token) {
      return static_cast<std::stop_token*>(token)->stop_requested() ? 1 : 0;
    },
    &stop);
}

Error
Failure(const std::exception& error, std::stop_token stop) {
  return stop.stop_requested()
           ? Error{ErrorCode::kCancelled, "Offline operation cancelled"}
           : Error{ErrorCode::kBackend,
               std::string("Catalog index: ") + error.what()};
}

nlohmann::json
Record(sqlite3_stmt* statement) {
  nlohmann::json record;
  int column = 0;
  for (const auto* field : {"id",
         "title",
         "manual_section",
         "description",
         "source_url",
         "family",
         "format",
         "file",
         "edition"})
    record[field] = Text(statement, column++);
  return record;
}
}  // namespace

Result<std::unique_ptr<CatalogIndex>>
CatalogIndex::Open(const std::filesystem::path& directory) {
  const auto missing = [] {
    return tl::make_unexpected(
      Error{ErrorCode::kNotFound, "No current catalog index"});
  };
  try {
    const auto file = directory / "catalog.sqlite3";
    if (!std::filesystem::is_regular_file(file))
      return missing();
    auto database = Connect(file);
    auto application = Prepare(database.get(), "PRAGMA application_id");
    auto version = Prepare(database.get(), "PRAGMA user_version");
    if (!Step(application.get()) ||
        sqlite3_column_int(application.get(), 0) != kApplicationId ||
        !Step(version.get()) || sqlite3_column_int(version.get(), 0) != 1)
      return tl::make_unexpected(Error{
        ErrorCode::kInvalidArgument, "Unsupported catalog index; rebuild it"});

    auto information = Prepare(database.get(),
      "SELECT catalog_bytes,catalog_mtime_ns,document_count FROM catalog_info");
    if (!Step(information.get()))
      return tl::make_unexpected(
        Error{ErrorCode::kInvalidArgument, "Catalog index has no fingerprint"});
    const auto catalog = directory / "catalog.json";
    const auto modified = std::filesystem::last_write_time(catalog);
    const auto modified_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
        decltype(modified)::clock::to_sys(modified).time_since_epoch())
        .count();
    const auto count = sqlite3_column_int64(information.get(), 2);
    if (count < 0 || count > 100000)
      return tl::make_unexpected(
        Error{ErrorCode::kInvalidArgument, "Catalog index is oversized"});
    if (sqlite3_column_int64(information.get(), 0) !=
          static_cast<sqlite3_int64>(std::filesystem::file_size(catalog)) ||
        sqlite3_column_int64(information.get(), 1) != modified_ns)
      return missing();

    auto index = std::unique_ptr<CatalogIndex>(new CatalogIndex());
    index->file_ = file;
    return index;
  } catch (const std::exception& error) {
    return tl::make_unexpected(Failure(error, {}));
  }
}

Result<void>
CatalogIndex::Rebuild(
  const std::filesystem::path& directory, std::stop_token stop) {
  try {
    if (stop.stop_requested())
      return tl::make_unexpected(
        Failure(std::runtime_error("Cancelled"), stop));
    const auto root = std::filesystem::weakly_canonical(directory);
    const auto catalog_path = root / "catalog.json";
    const auto catalog_size = std::filesystem::file_size(catalog_path);
    const auto modified = std::filesystem::last_write_time(catalog_path);
    if (catalog_size > 64 * 1024 * 1024)
      return tl::make_unexpected(
        Error{ErrorCode::kInvalidArgument, "Catalog exceeds 64 MiB"});
    std::ifstream input(catalog_path, std::ios::binary);
    std::string bytes(static_cast<std::size_t>(catalog_size), '\0');
    if (!input.read(bytes.data(), static_cast<std::streamsize>(bytes.size())))
      throw std::runtime_error("Could not read catalog.json");
    const auto catalog = nlohmann::json::parse(bytes);
    if (catalog.at("schema_version") != 1 ||
        !catalog.at("documents").is_array() ||
        catalog.at("documents").size() > 100000)
      throw std::runtime_error("Unsupported or oversized catalog");

    // Build privately first. Readers never see a partially populated index.
    auto database = Connect(":memory:", true);
    WatchCancellation(database.get(), stop);
    Check(database.get(),
      sqlite3_exec(database.get(),
        R"sql(
      PRAGMA application_id=0x4d414e42;
      PRAGMA user_version=1;
      CREATE TABLE catalog_info(catalog_bytes INTEGER NOT NULL,
        catalog_mtime_ns INTEGER NOT NULL,document_count INTEGER NOT NULL);
      CREATE TABLE documents(ordinal INTEGER PRIMARY KEY,id TEXT NOT NULL UNIQUE,
        title TEXT NOT NULL,manual_section TEXT NOT NULL,description TEXT NOT NULL,
        source_url TEXT NOT NULL,family TEXT NOT NULL,format TEXT NOT NULL,
        file TEXT NOT NULL,edition TEXT NOT NULL,source TEXT NOT NULL,label TEXT NOT NULL,
        title_key TEXT NOT NULL,id_key TEXT NOT NULL,label_key TEXT NOT NULL,
        search_text TEXT NOT NULL);
      CREATE INDEX document_titles ON documents(title);
      CREATE INDEX document_labels ON documents(label);
      CREATE INDEX document_files ON documents(file);
      CREATE INDEX document_urls ON documents(source_url);
      BEGIN;
    )sql",
        nullptr,
        nullptr,
        nullptr));
    auto insert = Prepare(database.get(),
      "INSERT INTO documents VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    std::size_t ordinal = 0;
    std::unordered_set<std::string> identities;
    for (const auto& record : catalog.at("documents")) {
      if (stop.stop_requested())
        return tl::make_unexpected(
          Failure(std::runtime_error("Cancelled"), stop));
      auto decoded = catalog_internal::DecodeRecord(record, root);
      if (!decoded)
        return tl::make_unexpected(decoded.error());
      const auto& page = decoded->metadata;
      if (!identities.insert(page.id).second)
        throw std::runtime_error("Duplicate offline document ID");
      const auto label = page.manual_section.empty()
                           ? page.title
                           : page.title + '(' + page.manual_section + ')';
      const std::vector<std::string> fields{page.id,
        page.title,
        page.manual_section,
        page.description,
        page.source_url,
        std::string(SourceFamilyName(page.family)),
        record.at("format"),
        record.at("file"),
        record.value("edition", ""),
        page.source,
        label,
        Lower(page.title),
        Lower(page.id),
        Lower(label),
        Lower(page.id + ' ' + label + ' ' + page.description + ' ' +
              page.source + ' ' + page.manual_section)};
      Check(database.get(),
        sqlite3_bind_int64(
          insert.get(), 1, static_cast<sqlite3_int64>(ordinal++)));
      for (std::size_t field = 0; field < fields.size(); ++field)
        Bind(insert.get(), static_cast<int>(field + 2), fields[field]);
      Step(insert.get());
      Check(database.get(), sqlite3_reset(insert.get()));
      Check(database.get(), sqlite3_clear_bindings(insert.get()));
    }
    if (std::filesystem::file_size(catalog_path) != catalog_size ||
        std::filesystem::last_write_time(catalog_path) != modified)
      throw std::runtime_error("Catalog changed while indexing; retry");
    const auto modified_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
        decltype(modified)::clock::to_sys(modified).time_since_epoch())
        .count();
    auto information =
      Prepare(database.get(), "INSERT INTO catalog_info VALUES(?,?,?)");
    Check(database.get(),
      sqlite3_bind_int64(
        information.get(), 1, static_cast<sqlite3_int64>(catalog_size)));
    Check(
      database.get(), sqlite3_bind_int64(information.get(), 2, modified_ns));
    Check(database.get(),
      sqlite3_bind_int64(
        information.get(), 3, static_cast<sqlite3_int64>(ordinal)));
    Step(information.get());
    Check(database.get(),
      sqlite3_exec(database.get(), "COMMIT", nullptr, nullptr, nullptr));
    if (stop.stop_requested())
      return tl::make_unexpected(
        Failure(std::runtime_error("Cancelled"), stop));

    catalog_internal::StagingDirectory staging(root);
    {
      auto output = Connect(staging.path() / "catalog.sqlite3", true);
      Check(output.get(), sqlite3_busy_timeout(output.get(), 1000));
      using Backup =
        std::unique_ptr<sqlite3_backup, decltype(&sqlite3_backup_finish)>;
      Backup backup(
        sqlite3_backup_init(output.get(), "main", database.get(), "main"),
        sqlite3_backup_finish);
      if (!backup)
        throw std::runtime_error(sqlite3_errmsg(output.get()));
      if (sqlite3_backup_step(backup.get(), -1) != SQLITE_DONE)
        throw std::runtime_error(sqlite3_errmsg(output.get()));
      Check(output.get(), sqlite3_backup_finish(backup.release()));
    }
    std::filesystem::rename(
      staging.path() / "catalog.sqlite3", root / "catalog.sqlite3");
    return {};
  } catch (const std::exception& error) {
    return tl::make_unexpected(Failure(error, stop));
  }
}

Result<std::vector<SearchHit>>
CatalogIndex::Search(std::string_view query, std::stop_token stop) const {
  if (query.size() > 4096)
    return tl::make_unexpected(
      Error{ErrorCode::kInvalidArgument, "Search query exceeds 4096 bytes"});
  try {
    std::string needle(query);
    for (auto& character : needle)
      if (character >= 'A' && character <= 'Z')
        character += 'a' - 'A';

    std::vector<std::string_view> words;
    std::size_t offset = 0;
    while (
      (offset = needle.find_first_not_of(' ', offset)) != std::string::npos) {
      const auto end = needle.find(' ', offset);
      words.push_back(std::string_view(needle).substr(offset, end - offset));
      if (end == std::string::npos)
        break;
      offset = end + 1;
    }
    if (words.size() > 256)
      return tl::make_unexpected(
        Error{ErrorCode::kInvalidArgument, "Search query exceeds 256 words"});

    std::string sql =
      "SELECT id,title,manual_section,description,source FROM documents "
      "WHERE ?1='' OR title_key=?1 OR id_key=?1 OR label_key=?1 "
      "OR instr(title_key,?1)>0 OR (1";
    for (std::size_t word = 0; word < words.size(); ++word)
      sql += " AND instr(search_text,?" + std::to_string(word + 2) + ")>0";
    sql +=
      ") ORDER BY CASE WHEN ?1='' THEN 1 "
      "WHEN title_key=?1 OR id_key=?1 OR label_key=?1 THEN 1000 "
      "WHEN substr(title_key,1,length(?1))=?1 THEN 750 "
      "WHEN instr(title_key,?1)>0 THEN 600 ELSE 300 END DESC, ordinal";

    auto database = Connect(file_);
    WatchCancellation(database.get(), stop);
    auto statement = Prepare(database.get(), sql);
    Bind(statement.get(), 1, needle);
    for (std::size_t word = 0; word < words.size(); ++word)
      Bind(statement.get(), static_cast<int>(word + 2), words[word]);

    std::vector<SearchHit> hits;
    while (Step(statement.get())) {
      if (stop.stop_requested())
        return tl::make_unexpected(
          Error{ErrorCode::kCancelled, "Offline operation cancelled"});
      hits.push_back({Text(statement.get(), 0),
        Text(statement.get(), 1),
        Text(statement.get(), 2),
        Text(statement.get(), 3),
        Text(statement.get(), 4)});
    }
    return hits;
  } catch (const std::exception& error) {
    return tl::make_unexpected(Failure(error, stop));
  }
}

Result<nlohmann::json>
CatalogIndex::Find(std::string_view reference, std::stop_token stop) const {
  if (reference.size() > 4096)
    return tl::make_unexpected(
      Error{ErrorCode::kInvalidArgument, "Page reference exceeds 4096 bytes"});
  try {
    auto database = Connect(file_);
    WatchCancellation(database.get(), stop);
    auto exact =
      Prepare(database.get(), std::string(kRecordFields) + "id=?1 LIMIT 1");
    Bind(exact.get(), 1, reference);
    if (Step(exact.get()))
      return Record(exact.get());

    auto named = Prepare(database.get(),
      std::string(kRecordFields) + "title=?1 OR label=?1 LIMIT 2");
    Bind(named.get(), 1, reference);
    if (!Step(named.get()))
      return tl::make_unexpected(
        Error{ErrorCode::kNotFound, "No such offline page"});
    auto record = Record(named.get());
    if (Step(named.get()))
      return tl::make_unexpected(Error{ErrorCode::kAmbiguous,
        "Ambiguous page name; specify name(section) or a page ID"});
    return record;
  } catch (const std::exception& error) {
    return tl::make_unexpected(Failure(error, stop));
  }
}

Result<nlohmann::json>
CatalogIndex::FindSource(
  std::string_view value, bool by_file, std::stop_token stop) const {
  if (value.size() > 4096)
    return tl::make_unexpected(
      Error{ErrorCode::kInvalidArgument, "Reference exceeds 4096 bytes"});
  try {
    auto database = Connect(file_);
    WatchCancellation(database.get(), stop);
    auto statement = Prepare(database.get(),
      std::string(kRecordFields) +
        (by_file ? "file=?1 LIMIT 2" : "source_url=?1 LIMIT 2"));
    Bind(statement.get(), 1, value);
    if (!Step(statement.get()))
      return tl::make_unexpected(Error{ErrorCode::kNotFound,
        "This reference is not available in the offline library"});
    auto record = Record(statement.get());
    if (Step(statement.get()))
      return tl::make_unexpected(Error{
        ErrorCode::kAmbiguous, "Reference matches multiple installed pages"});
    return record;
  } catch (const std::exception& error) {
    return tl::make_unexpected(Failure(error, stop));
  }
}
}  // namespace manbo
