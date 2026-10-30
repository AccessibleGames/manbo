#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <ranges>

#include "repositories/offline_repository.h"
#include "support/fixtures.h"

namespace manbo {
namespace {
using test::Bytes;
using test::ContentText;

struct SourceCase {
  const char* id;
  const char* file;
  SourceFamily family;
  const char* expected;
};

class RealCorpus : public testing::TestWithParam<SourceCase> {};
TEST_P(RealCorpus, AdaptsOriginalBytesOfflineWithoutChromeOrSourceChanges) {
  const auto root = std::filesystem::path(MANBO_TEST_CORPUS);
  if (!std::filesystem::exists(root / "catalog.json"))
    GTEST_SKIP() << "Optional authentic corpus is absent; import local HTML "
                    "collections for manual review";
  const auto& sample = GetParam();
  const auto original = Bytes(root / sample.file);
  ASSERT_FALSE(original.empty());
  auto repository = OfflineRepository::Open(root);
  ASSERT_TRUE(repository) << (repository ? "" : repository.error().message);
  auto page = (*repository)->Read(sample.id, {});
  ASSERT_TRUE(page) << (page ? "" : page.error().message);
  ASSERT_TRUE(page->content);
  EXPECT_EQ(page->body, original);
  EXPECT_EQ(page->family, sample.family);
  EXPECT_TRUE(page->source_url.starts_with("https://"));
  EXPECT_GT(page->content->blocks.size(), 10u);
  const auto text = ContentText(*page->content);
  EXPECT_NE(text.find(sample.expected), std::string::npos);
  EXPECT_EQ(text.find("sc_project="), std::string::npos);
  EXPECT_EQ(text.find("Next >>>"), std::string::npos);
  EXPECT_EQ(text.find("[edit]"), std::string::npos);
  EXPECT_EQ(
    text.find("Linux/UNIX system programming training"), std::string::npos);
  if (sample.family == SourceFamily::kCppReference) {
    EXPECT_TRUE(
      std::ranges::any_of(page->content->blocks, [](const auto& block) {
        return block.kind == BlockKind::kCode && block.language == "cpp";
      }));
    const std::string expected_paragraph =
      std::string_view(sample.id) == "cpp/insert-after"
        ? "Inserts value after the element pointed to by pos."
        : "All member functions of std::forward_list are constexpr";
    EXPECT_TRUE(
      std::ranges::any_of(page->content->blocks, [&](const auto& block) {
        return block.kind == BlockKind::kParagraph &&
               InlineText(block.text).find(expected_paragraph) !=
                 std::string::npos;
      }));
    EXPECT_TRUE(
      std::ranges::any_of(page->content->blocks, [](const auto& block) {
        return block.kind == BlockKind::kCode &&
               block.annotation.find("since C++11") != std::string::npos;
      }));
    EXPECT_EQ(text.find("Containers library"), std::string::npos);
    EXPECT_EQ(text.find("Contents"), std::string::npos);
  } else if (sample.family == SourceFamily::kLinuxMan) {
    EXPECT_TRUE(std::ranges::none_of(page->content->blocks,
      [](const auto& block) { return block.kind == BlockKind::kCode; }));
    EXPECT_TRUE(
      std::ranges::any_of(page->content->blocks, [](const auto& block) {
        return block.kind == BlockKind::kHeading &&
               InlineText(block.text) == "NAME";
      }));
  } else if (sample.family == SourceFamily::kGnu)
    EXPECT_EQ(text.find("Next:"), std::string::npos);
  for (const int width : {40, 80, 132}) {
    Snapshot state;
    state.route = Route::kReader;
    state.document = *page;
    const auto frame = RenderFrame(state, width, 42);
    for (const auto& line : PlainLines(frame))
      EXPECT_LE(TextColumns(line), static_cast<std::size_t>(width));
    EXPECT_EQ(SerializeHtml(frame).find("<script>"), std::string::npos);
  }

  EXPECT_EQ(Bytes(root / sample.file), original);
}

INSTANTIATE_TEST_SUITE_P(Official,
  RealCorpus,
  testing::Values(SourceCase{"cpp/forward-list",
                    "cpp-forward-list.html",
                    SourceFamily::kCppReference,
                    "forward_list"},
    SourceCase{"cpp/insert-after",
      "cpp-insert-after.html",
      SourceFamily::kCppReference,
      "insert_after"},
    SourceCase{"posix/socket/3p",
      "posix-socket.html",
      SourceFamily::kPosix,
      "SOCK_STREAM"},
    SourceCase{"posix/printf/1p",
      "posix-printf.html",
      SourceFamily::kPosix,
      "EXTENDED DESCRIPTION"},
    SourceCase{"gnu/printf/1",
      "gnu-printf.html",
      SourceFamily::kGnu,
      "escape sequences"},
    SourceCase{"linux/printf/3",
      "linux-printf.html",
      SourceFamily::kLinuxMan,
      "RETURN VALUE"},
    SourceCase{"linux/socket/2",
      "linux-socket.html",
      SourceFamily::kLinuxMan,
      "SOCK_STREAM"},
    SourceCase{
      "linux/ls/1", "linux-ls.html", SourceFamily::kLinuxMan, "--all"}));

}  // namespace
}  // namespace manbo
