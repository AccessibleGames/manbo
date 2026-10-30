#include <fmt/format.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>

#include "repositories/link_target.h"
#include "repositories/offline_repository.h"
#include "support/fixtures.h"

namespace manbo {
namespace {
using test::Bytes;
using test::ContentText;

class CatalogFixture : public testing::Test {
 protected:
  void
  SetUp() override {
    root = std::filesystem::temp_directory_path() /
           ("manbo-catalog-" +
             std::to_string(
               std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(root);
  }

  void
  TearDown() override {
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }

  std::string
  Entry(std::string_view file = "source.html",
    std::string_view family = "generic",
    std::string_view format = "html") {
    return fmt::format(
      R"({{"id":"test/page","title":"page","manual_section":"3p","description":"local test","family":"{}","format":"{}","file":"{}","source_url":"https://example.invalid/owned-fixture"}})",
      family,
      format,
      file);
  }

  void
  Catalog(std::string entries) {
    std::ofstream file(root / "catalog.json", std::ios::binary);
    file << "{\"schema_version\":1,\"documents\":[" << entries << "]}";
  }
  std::filesystem::path root;
};
TEST(ReferenceTargets, CanonicalCppreferenceUrlsMapToTheSameOfflineBook) {
  Document origin;
  origin.family = SourceFamily::kCppReference;
  origin.source_file =
    std::filesystem::path("/library/cpp/container/vector.html");
  origin.source_url = "https://en.cppreference.com/w/cpp/container/vector.html";
  for (const std::string reference :
    {"https://en.cppreference.com/w/cpp/container/vector/push_back#Example",
      "/w/cpp/container/vector/push_back#Example"}) {
    auto target = ResolveLink(origin, reference);
    ASSERT_TRUE(target);
    EXPECT_EQ(target->anchor, "Example");
    EXPECT_NE(
      std::ranges::find(target->files,
        std::filesystem::path("/library/cpp/container/vector/push_back.html")),
      target->files.end());
  }
  const auto external =
    ResolveLink(origin, "https://other.invalid/w/cpp/container/vector");
  ASSERT_TRUE(external);
  EXPECT_TRUE(external->files.empty());
}

TEST_F(CatalogFixture, PathsDuplicatesAndUnknownFamiliesAreRejected) {
  Catalog(Entry("../outside.html"));
  EXPECT_FALSE(OfflineRepository::Open(root));
  Catalog(Entry("C:/outside.html"));
  EXPECT_FALSE(OfflineRepository::Open(root));
  Catalog(Entry("source.html", "mystery"));
  EXPECT_FALSE(OfflineRepository::Open(root));
  Catalog(Entry("source.html", "generic", "roff"));
  EXPECT_FALSE(OfflineRepository::Open(root));
  Catalog(Entry() + ',' + Entry());
  EXPECT_FALSE(OfflineRepository::Open(root));
}

TEST_F(CatalogFixture, FullLibrariesExceedTheOldPageAndByteLimits) {
  std::string entries;
  for (int number = 0; number < 1500; ++number) {
    if (!entries.empty())
      entries += ',';
    entries += fmt::format(
      R"({{"id":"test/{}","title":"page{}","description":"{}","family":"generic","format":"html","file":"source.html","source_url":"https://example.invalid/owned-fixture"}})",
      number,
      number,
      std::string(800, 'x'));
  }

  ASSERT_GT(entries.size(), 1024 * 1024);
  Catalog(std::move(entries));
  auto repository = OfflineRepository::Open(root);
  ASSERT_TRUE(repository);
  auto all_pages = (*repository)->Search("", {});
  ASSERT_TRUE(all_pages);
  EXPECT_EQ(all_pages->size(), 1500);
  auto exact = (*repository)->Search("page1499", {});
  ASSERT_TRUE(exact);
  ASSERT_EQ(exact->size(), 1);
  EXPECT_EQ(exact->front().id, "test/1499");
}

TEST_F(CatalogFixture, FormattedManTextKeepsItsFamilyAndSourceBytes) {
  const std::string formatted =
    "NAME\n       page - formatted manual\n\nSYNOPSIS\n"
    "       int page(void);\n\nDESCRIPTION\n       Read this manual\n"
    "       with wrapped prose.\n\n       -a\n           an option\n";
  Catalog(Entry("page.txt", "linux-man", "plain"));
  {
    std::ofstream file(root / "page.txt", std::ios::binary);
    file << formatted;
  }

  auto repository = OfflineRepository::Open(root);
  ASSERT_TRUE(repository);
  auto page = (*repository)->Read("page(3p)", {});
  ASSERT_TRUE(page);
  EXPECT_EQ(page->family, SourceFamily::kLinuxMan);
  EXPECT_EQ(page->format, SourceFormat::kPlainText);
  EXPECT_EQ(page->body, formatted);
  EXPECT_NE(
    ContentText(*page->content).find("int page(void);"), std::string::npos);
  EXPECT_NE(
    ContentText(*page->content).find("Read this manual with wrapped prose."),
    std::string::npos);
  const auto& option = page->content->blocks.back();
  EXPECT_EQ(option.kind, BlockKind::kPreformatted);
  EXPECT_EQ(InlineText(option.text), "-a\n    an option");
  EXPECT_EQ(Bytes(root / "page.txt"), formatted);
}

TEST_F(
  CatalogFixture, MissingOversizedCancelledAndMalformedInputsAreTypedFailures) {
  Catalog(Entry());
  auto repository = OfflineRepository::Open(root);
  ASSERT_TRUE(repository);
  auto missing = (*repository)->Read("page(3p)", {});
  ASSERT_FALSE(missing);
  EXPECT_EQ(missing.error().code, ErrorCode::kNotFound);
  {
    std::ofstream file(root / "source.html", std::ios::binary);
    file << std::string(2 * 1024 * 1024 + 1, 'x');
  }

  auto large = (*repository)->Read("page", {});
  ASSERT_FALSE(large);
  EXPECT_EQ(large.error().code, ErrorCode::kInvalidArgument);
  std::stop_source stop;
  stop.request_stop();
  EXPECT_EQ((*repository)->Read("page", stop.get_token()).error().code,
    ErrorCode::kCancelled);
  EXPECT_EQ((*repository)->Search("page", stop.get_token()).error().code,
    ErrorCode::kCancelled);
  {
    std::ofstream file(root / "catalog.json");
    file << "not JSON";
  }

  auto malformed = OfflineRepository::Open(root);
  ASSERT_FALSE(malformed);
  EXPECT_EQ(malformed.error().code, ErrorCode::kInvalidArgument);
}

TEST_F(CatalogFixture, SourceBytesAndSectionLabelsStaySeparateFromRichContent) {
  const std::string original =
    "<main><h1>page</h1><p>Read <b>carefully</b>.</p></main>\r\n";
  Catalog(Entry());
  {
    std::ofstream file(root / "source.html", std::ios::binary);
    file << original;
  }

  auto repository = OfflineRepository::Open(root);
  ASSERT_TRUE(repository);
  auto page = (*repository)->Read("page(3p)", {});
  ASSERT_TRUE(page);
  EXPECT_EQ(page->manual_section, "3p");
  EXPECT_EQ(page->body, original);
  EXPECT_EQ(ContentText(*page->content).find("3p"), std::string::npos);
  EXPECT_EQ(Bytes(root / "source.html"), original);
}

}  // namespace
}  // namespace manbo
