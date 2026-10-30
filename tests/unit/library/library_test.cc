#include "library/library.h"

#include <gtest/gtest.h>

#include <fstream>
#include <nlohmann/json.hpp>

#include "repositories/catalog_staging.h"
#include "repositories/offline_repository.h"

namespace manbo {
namespace {
using Json = nlohmann::json;

std::string
Bytes(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {
    std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void
Write(const std::filesystem::path& path, std::string_view text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary);
  output << text;
  ASSERT_TRUE(output);
}

std::vector<std::string>
Identities(const std::vector<SearchHit>& hits) {
  std::vector<std::string> result;
  for (const auto& hit : hits)
    result.push_back(hit.id);
  return result;
}

class LocalLibrary : public testing::Test {
 protected:
  LocalLibrary() : temporary_(std::filesystem::temp_directory_path()) {
    options.source = temporary_.path() / "originals";
    options.destination = temporary_.path() / "library";
    options.pack = "guide-v1";
    options.edition = "1.0";
    options.source_url = "https://example.invalid/guide/";
    Write(options.source / "index.html", first);
    Write(options.source / "api/open.html", second);
    Write(options.source / "NOTICE", "abc");
    Write(options.source / "style.css", "p { color: inherit; }");
  }

  Json
  Catalog() const {
    return Json::parse(Bytes(options.destination / "catalog.json"));
  }

  catalog_internal::StagingDirectory temporary_;
  ImportOptions options;
  const std::string first =
    "<html><head><title>Guide</title></head><body><main><h1>Guide</h1>"
    "<p>Endpoint communication.</p><a href='api/open.html#Example'>Open</a>"
    "</main></body></html>";
  const std::string second =
    "<html><head><title>open</title></head><body><main>"
    "<h2 id='Example'>Example</h2><p>Open a file.</p></main></body></html>";
};

TEST_F(LocalLibrary, ImportPreservesHierarchyAssetsAndReferences) {
  const auto imported = ImportDocuments(options);
  ASSERT_TRUE(imported) << imported.error().message;
  EXPECT_EQ(imported->imported, 2u);
  EXPECT_EQ(imported->total, 2u);
  const auto sources = options.destination / "sources/guide-v1";
  EXPECT_EQ(Bytes(sources / "index.html"), first);
  EXPECT_EQ(Bytes(sources / "api/open.html"), second);
  EXPECT_EQ(Bytes(options.source / "index.html"), first);
  EXPECT_EQ(Bytes(sources / "NOTICE"), "abc");
  const auto manifest =
    Json::parse(Bytes(options.destination / "manifests/guide-v1.json"));
  EXPECT_EQ(manifest["files"]["NOTICE"],
    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  auto repository = OfflineRepository::Open(options.destination);
  ASSERT_TRUE(repository);
  const auto hits = (*repository)->Search("Guide", {});
  ASSERT_TRUE(hits);
  ASSERT_FALSE(hits->empty());
  EXPECT_EQ(hits->front().id, "guide-v1/index");
  EXPECT_EQ(hits->front().description, "Endpoint communication.");
  EXPECT_NE(hits->front().source.find("1.0"), std::string::npos);
  const auto linked =
    (*repository)->Follow("guide-v1/index", "api/open.html#Example", {});
  ASSERT_TRUE(linked) << linked.error().message;
  EXPECT_EQ(linked->document.body, second);
  EXPECT_EQ(linked->anchor, "Example");
}

TEST_F(LocalLibrary, ReimportIsIdempotentAndChangedEditionsUseNewPackNames) {
  ASSERT_TRUE(ImportDocuments(options));
  const auto before = Bytes(options.destination / "catalog.json");
  ASSERT_TRUE(ImportDocuments(options));
  EXPECT_EQ(Bytes(options.destination / "catalog.json"), before);
  Write(options.source / "index.html", first + "<p>A new edition.</p>");
  EXPECT_FALSE(ImportDocuments(options));
  EXPECT_EQ(Bytes(options.destination / "catalog.json"), before);
  EXPECT_EQ(Bytes(options.destination / "sources/guide-v1/index.html"), first);
  options.pack = "guide-v2";
  options.edition = "2.0";
  const auto second_edition = ImportDocuments(options);
  ASSERT_TRUE(second_edition);
  EXPECT_EQ(second_edition->total, 4u);
  EXPECT_EQ(Catalog()["documents"].size(), 4u);
  auto repository = OfflineRepository::Open(options.destination);
  ASSERT_TRUE(repository);
  const auto ambiguous = (*repository)->Read("Guide", {});
  ASSERT_FALSE(ambiguous);
  EXPECT_EQ(ambiguous.error().code, ErrorCode::kAmbiguous);
}

TEST_F(LocalLibrary, MetadataSearchMatchesJsonAndCorruptedIndexesCanBeRebuilt) {
  ASSERT_TRUE(ImportDocuments(options));
  const std::vector<std::string> queries{"",
    "open",
    "OPEN",
    "Guide",
    "guide-v1/index",
    "endpoint communication",
    "file open",
    "no match",
    "%",
    "_",
    " "};
  std::vector<std::vector<std::string>> expected;
  {
    auto repository = OfflineRepository::Open(options.destination);
    ASSERT_TRUE(repository);
    for (const auto& query : queries)
      expected.push_back(Identities(*(*repository)->Search(query, {})));
  }
  std::filesystem::remove(options.destination / "catalog.sqlite3");
  {
    auto repository = OfflineRepository::Open(options.destination);
    ASSERT_TRUE(repository);
    for (std::size_t index = 0; index < queries.size(); ++index)
      EXPECT_EQ(Identities(*(*repository)->Search(queries[index], {})),
        expected[index]);
  }
  Write(options.destination / "catalog.sqlite3", "corrupted cache");
  ASSERT_TRUE(RebuildCatalogIndex(options.destination));
  auto repository = OfflineRepository::Open(options.destination);
  ASSERT_TRUE(repository);
  EXPECT_EQ(Identities(*(*repository)->Search("", {})), expected[0]);
}

TEST_F(
  LocalLibrary, StaleCatalogsFallBackAndInvalidRebuildsPreserveTheOldIndex) {
  ASSERT_TRUE(ImportDocuments(options));
  auto catalog = Catalog();
  catalog["documents"][0]["title"] = "renamed command";
  Write(options.destination / "catalog.json", catalog.dump(2));
  auto repository = OfflineRepository::Open(options.destination);
  ASSERT_TRUE(repository);
  EXPECT_EQ((*repository)->Search("renamed command", {})->size(), 1u);
  ASSERT_TRUE(RebuildCatalogIndex(options.destination));
  const auto index_before = Bytes(options.destination / "catalog.sqlite3");
  catalog["documents"][0]["file"] = "../escape.html";
  Write(options.destination / "catalog.json", catalog.dump(2));
  EXPECT_FALSE(RebuildCatalogIndex(options.destination));
  EXPECT_EQ(Bytes(options.destination / "catalog.sqlite3"), index_before);
  catalog["documents"][0]["file"] = "sources/guide-v1/api/open.html";
  catalog["documents"].push_back(catalog["documents"][0]);
  Write(options.destination / "catalog.json", catalog.dump(2));
  EXPECT_FALSE(RebuildCatalogIndex(options.destination));
  EXPECT_EQ(Bytes(options.destination / "catalog.sqlite3"), index_before);
}

TEST_F(LocalLibrary, InvalidInputsAndCancellationDoNotReplaceTheCatalog) {
  ASSERT_TRUE(ImportDocuments(options));
  const auto before = Bytes(options.destination / "catalog.json");
  options.pack = "../escape";
  EXPECT_FALSE(ImportDocuments(options));
  options.pack = "bad";
  options.source_url = "file:///etc/passwd";
  EXPECT_FALSE(ImportDocuments(options));
  options.source_url.clear();
  std::stop_source cancelled;
  cancelled.request_stop();
  const auto stopped = ImportDocuments(options, cancelled.get_token());
  ASSERT_FALSE(stopped);
  EXPECT_EQ(stopped.error().code, ErrorCode::kCancelled);
  Write(
    options.source / "oversized.html", std::string(2 * 1024 * 1024 + 1, 'x'));
  EXPECT_FALSE(ImportDocuments(options));
  EXPECT_EQ(Bytes(options.destination / "catalog.json"), before);
  options.destination = options.source / "nested-library";
  EXPECT_FALSE(ImportDocuments(options));
}

TEST_F(LocalLibrary, PageSubtreesKeepBookAssetsAndUsePageRelativeIdentities) {
  options.pages = "reference/en";
  Write(options.source / "reference/en/index.html", first);
  Write(options.source / "reference/en/api/open.html", second);
  Write(options.source / "reference/common/style.css", "shared book assets");
  ASSERT_TRUE(ImportDocuments(options));
  EXPECT_EQ(Catalog()["documents"].size(), 2u);
  EXPECT_EQ(
    Bytes(options.destination / "sources/guide-v1/reference/common/style.css"),
    "shared book assets");
  auto repository = OfflineRepository::Open(options.destination);
  ASSERT_TRUE(repository);
  const auto page = (*repository)->Read("guide-v1/index", {});
  ASSERT_TRUE(page);
  EXPECT_EQ(page->source_url, "https://example.invalid/guide/index.html");
  const auto linked =
    (*repository)->Follow(page->id, "api/open.html#Example", {});
  ASSERT_TRUE(linked);
  EXPECT_EQ(linked->document.id, "guide-v1/api/open");
  options.pages = "../outside";
  EXPECT_FALSE(ImportDocuments(options));
}

TEST_F(LocalLibrary, Utf8PathsAndTitlesRoundTripThroughTheIndex) {
  Write(options.source / std::filesystem::u8path("章节/打开.html"),
    "<html><head><title>打开</title></head><body><main><p>本地文档。</p></"
    "main></body></html>");
  ASSERT_TRUE(ImportDocuments(options));
  auto repository = OfflineRepository::Open(options.destination);
  ASSERT_TRUE(repository);
  const auto page = (*repository)->Read("guide-v1/章节/打开", {});
  ASSERT_TRUE(page);
  EXPECT_EQ(page->title, "打开");
  EXPECT_EQ((*repository)->Search("打开", {})->size(), 1u);
  EXPECT_NE(page->body.find("本地文档。"), std::string::npos);
}

TEST_F(LocalLibrary, EmptyHtmlIsReportedWithoutPublishingAnEmptyPage) {
  Write(options.source / "empty.html",
    "<html><head><title>Empty</title></head><body></body></html>");
  const auto result = ImportDocuments(options);
  ASSERT_FALSE(result);
  EXPECT_NE(result.error().message.find("no visible document content"),
    std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(options.destination / "catalog.json"));
  EXPECT_EQ(Bytes(options.source / "index.html"), first);
}

TEST_F(LocalLibrary, FormattedManualsKeepSectionIdentityAndReferences) {
  std::filesystem::remove_all(options.source);
  Write(options.source / "socket.2.txt",
    "NAME\n       socket - create an endpoint\n\n"
    "SEE ALSO\n       accept(2),\n       close(2).\n");
  Write(options.source / "accept.2.txt",
    "NAME\n       accept - accept a connection\n");
  options.format = SourceFormat::kPlainText;
  options.family = SourceFamily::kLinuxMan;
  ASSERT_TRUE(ImportDocuments(options));
  auto repository = OfflineRepository::Open(options.destination);
  ASSERT_TRUE(repository);
  const auto socket = (*repository)->Read("socket(2)", {});
  ASSERT_TRUE(socket);
  const auto accepted = (*repository)->Follow(socket->id, "accept(2)", {});
  ASSERT_TRUE(accepted);
  EXPECT_EQ(accepted->document.title, "accept");
  EXPECT_EQ(accepted->document.manual_section, "2");
  bool linked_close = false;
  for (const auto& block : socket->content->blocks)
    for (const auto& run : block.text)
      linked_close = linked_close || run.link == "close(2)";
  EXPECT_TRUE(linked_close);
}
}  // namespace
}  // namespace manbo
