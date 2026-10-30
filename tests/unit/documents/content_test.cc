#include "documents/content.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <ranges>

#include "support/fixtures.h"

namespace manbo {
namespace {
using test::FrameText;
using test::HasRole;
using test::HtmlDocument;
using test::ReaderState;
using test::SamplePage;

TEST(Content, PlainSourceIsPreservedAndAdaptedBeforeRendering) {
  const auto page = SamplePage();
  ASSERT_TRUE(page.content);
  EXPECT_TRUE(page.body.starts_with("NAME\n  std::forward_list"));
  EXPECT_EQ(page.body.find('\x1b'), std::string::npos);
  EXPECT_TRUE(std::ranges::any_of(page.content->blocks, [](const auto& block) {
    return block.kind == BlockKind::kHeading &&
           InlineText(block.text) == "SYNOPSIS" && block.source_line > 0;
  }));
  EXPECT_TRUE(std::ranges::any_of(page.content->blocks, [](const auto& block) {
    return block.kind == BlockKind::kCode && block.language == "cpp" &&
           InlineText(block.text).find("#include <forward_list>") !=
             std::string::npos;
  }));
}

TEST(
  Content, HtmlRetainsSemanticBlocksAndNestedInlineStylesWithoutEditingSource) {
  const std::string source =
    "<main><h1 id='name'>Fixture</h1><p>Use <strong><em>care</em></strong> "
    "and "
    "<code>std::vector</code> with <a href='#name'>links</a>.</p>"
    "<ul><li>First item</li></ul><blockquote>Note</blockquote>"
    "<table><tr><th>Key</th><th>Meaning</th></tr><tr><td>A</td><td>Value</"
    "td></tr></table></main>";
  auto page = HtmlDocument(source);
  ASSERT_TRUE(page.content);
  EXPECT_EQ(page.body, source);
  bool nested = false;
  bool link = false;
  bool code = false;
  bool table = false;
  for (const auto& block : page.content->blocks) {
    table = table || (block.kind == BlockKind::kTable &&
                       block.rows.size() == 2 && block.rows[0].header);
    for (const auto& run : block.text) {
      nested = nested || (run.strong && run.emphasis && run.text == "care");
      link = link || run.link == "#name";
      code = code || (run.code && run.text == "std::vector");
    }
  }

  EXPECT_TRUE(nested);
  EXPECT_TRUE(link);
  EXPECT_TRUE(code);
  EXPECT_TRUE(table);
}

TEST(Content, InlineSiblingsStayInTheirParagraphAcrossContainerWrappers) {
  const auto page = HtmlDocument(
    "<div>Inserts <span><code>value</code></span> after the element "
    "pointed to by <i>pos</i>.</div>"
    "<section><p>Next paragraph.</p></section>");
  ASSERT_TRUE(page.content);
  ASSERT_EQ(page.content->blocks.size(), 2u);
  EXPECT_EQ(InlineText(page.content->blocks[0].text),
    "Inserts value after the element pointed to by pos.");
  EXPECT_EQ(InlineText(page.content->blocks[1].text), "Next paragraph.");
  EXPECT_TRUE(std::ranges::any_of(page.content->blocks[0].text,
    [](const auto& run) { return run.code && run.text == "value"; }));
}

TEST(Content, PreformattedManualHtmlKeepsIndentationBoldAndItalic) {
  const std::string source =
    "<main><pre><b>NAME</b>\n       printf - output\n\n"
    "<b>int printf(const char *</b><i>format</i><b>);</b>\n"
    "       <u>argument</u> <a "
    "href='printf.3.html'>printf(3)</a>\n</pre></main>";
  auto page = HtmlDocument(source);
  ASSERT_TRUE(page.content);
  ASSERT_EQ(page.content->blocks.size(), 1u);
  EXPECT_EQ(page.content->blocks[0].kind, BlockKind::kPreformatted);
  EXPECT_EQ(page.body, source);
  const auto frame = RenderFrame(ReaderState(page), 100, 36);
  EXPECT_NE(FrameText(frame).find("       printf - output"), std::string::npos);
  bool strong = false;
  bool italic = false;
  bool underline = false;
  for (const auto& line : frame.lines)
    for (const auto& run : line) {
      strong =
        strong || (run.bold && run.text.find("NAME") != std::string::npos);
      italic =
        italic || (run.italic && run.text.find("format") != std::string::npos);
      underline = underline || (run.underline && run.text.find("argument") !=
                                                   std::string::npos);
    }
  EXPECT_TRUE(strong);
  EXPECT_TRUE(italic);
  EXPECT_TRUE(underline);
  EXPECT_FALSE(HasRole(frame, StyleRole::kKeyword));
}

TEST(Content, ScriptChromeAndUnsafeLinkSchemesDoNotBecomeDocumentContent) {
  auto page = HtmlDocument(
    "<html><head><title>HEAD_CANARY</title></head><body><nav>NAV_CANARY</nav>"
    "<main><h1>Fixture</h1><p>Text <a href='javascript:evil()'>label</a></p>"
    "<script>SCRIPT_CANARY</script><iframe "
    "src='https://invalid.example/'>FRAME_CANARY</iframe>"
    "<style>STYLE_CANARY</style></main><footer>FOOTER_CANARY</footer></"
    "body></html>");
  ASSERT_TRUE(page.content);
  std::string visible;
  for (const auto& block : page.content->blocks) {
    visible += InlineText(block.text);
    for (const auto& run : block.text)
      EXPECT_TRUE(run.link.empty());
  }

  EXPECT_NE(visible.find("label"), std::string::npos);
  EXPECT_EQ(visible.find("CANARY"), std::string::npos);
}

TEST(Content, ExternalEntitiesAreNotExpandedFromOwnedLocalFiles) {
  const auto path =
    std::filesystem::temp_directory_path() /
    ("manbo-owned-entity-" +
      std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()) +
      ".txt");
  const std::string marker = "MANBO_OWNED_ENTITY_MUST_NOT_BE_LOADED";
  {
    std::ofstream file(path);
    file << marker;
  }
  const auto source = "<!DOCTYPE html [<!ENTITY stolen SYSTEM 'file:///" +
                      path.generic_string() +
                      "'>]>"
                      "<html><body><main><p>&stolen;</p></main></body></html>";
  auto parsed = AdaptContent({source, SourceFormat::kHtml, ""});
  std::error_code error;
  std::filesystem::remove(path, error);
  ASSERT_TRUE(parsed);
  for (const auto& block : (*parsed)->blocks)
    EXPECT_EQ(InlineText(block.text).find(marker), std::string::npos);
}

TEST(Content, LimitsCancellationAndUnimplementedRoffReturnTypedErrors) {
  auto roff = AdaptContent({".TH NAME 1\n.SH NAME", SourceFormat::kRoff, ""});
  ASSERT_FALSE(roff);
  EXPECT_NE(roff.error().message.find("formatter"), std::string::npos);
  const std::string large(2 * 1024 * 1024 + 1, 'x');
  EXPECT_FALSE(AdaptContent({large, SourceFormat::kHtml, ""}));
  std::stop_source stop;
  stop.request_stop();
  auto cancelled =
    AdaptContent({"<p>Hello</p>", SourceFormat::kHtml, ""}, stop.get_token());
  ASSERT_FALSE(cancelled);
  EXPECT_EQ(cancelled.error().code, ErrorCode::kCancelled);
}
TEST(SourceAdapters, FamiliesRejectWrongSourcesAndDoNotPretendRoffIsHtml) {
  for (const auto family : {SourceFamily::kCppReference,
         SourceFamily::kPosix,
         SourceFamily::kGnu,
         SourceFamily::kLinuxMan}) {
    EXPECT_FALSE(AdaptContent({"<main><p>Not that source</p></main>",
      SourceFormat::kHtml,
      "",
      family}));
    auto roff = AdaptContent({".TH NAME 1", SourceFormat::kRoff, "", family});
    ASSERT_FALSE(roff);
    EXPECT_NE(roff.error().message.find("formatter"), std::string::npos);
    EXPECT_EQ(*ParseSourceFamily(SourceFamilyName(family)), family);
  }

  EXPECT_FALSE(ParseSourceFamily("unrecognized-source"));
}

TEST(SourceAdapters,
  DepthNodesAndTableBoundsReturnErrorsInsteadOfTruncatedSuccess) {
  std::string deep = "<main>";
  for (int n = 0; n < 140; ++n)
    deep += "<div>";
  deep += "Text";
  for (int n = 0; n < 140; ++n)
    deep += "</div>";
  deep += "</main>";
  EXPECT_FALSE(AdaptContent({deep, SourceFormat::kHtml, ""}));
  std::string many = "<main>";
  for (int n = 0; n < 55000; ++n)
    many += "<p>x</p>";
  many += "</main>";
  EXPECT_FALSE(AdaptContent({many, SourceFormat::kHtml, ""}));
  std::string columns = "<table><tr>";
  for (int n = 0; n < 33; ++n)
    columns += "<td>x</td>";
  columns += "</tr></table>";
  EXPECT_FALSE(AdaptContent({columns, SourceFormat::kHtml, ""}));
}

TEST(SourceAdapters, PosixChaptersUseTheOfficialEditionBanner) {
  const std::string chapter =
    "<html><body><center>The Open Group Base Specifications Issue 8"
    "</center><h2>1. Introduction</h2><p>POSIX.1-2024 interfaces.</p>"
    "</body></html>";
  auto content =
    AdaptContent({chapter, SourceFormat::kHtml, "c", SourceFamily::kPosix});
  ASSERT_TRUE(content);
  EXPECT_GE((*content)->blocks.size(), 2);
}

TEST(Content, LargeReferenceTablesKeepAllRowsWithinABoundedLimit) {
  std::string rows;
  for (int number = 0; number < 300; ++number)
    rows += "<tr><td>feature</td><td>value</td></tr>";

  auto content = AdaptContent(
    {"<main><table>" + rows + "</table></main>", SourceFormat::kHtml, ""});
  ASSERT_TRUE(content);
  ASSERT_EQ((*content)->blocks.size(), 1);
  EXPECT_EQ((*content)->blocks.front().rows.size(), 300);

  for (int number = 300; number < 2049; ++number)
    rows += "<tr><td>feature</td><td>value</td></tr>";
  EXPECT_FALSE(AdaptContent(
    {"<main><table>" + rows + "</table></main>", SourceFormat::kHtml, ""}));
}

}  // namespace
}  // namespace manbo
