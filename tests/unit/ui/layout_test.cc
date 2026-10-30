#include <gtest/gtest.h>

#include <algorithm>
#include <ranges>
#include <regex>

#include "core/browser.h"
#include "support/fixtures.h"

namespace manbo {
namespace {
using test::ExampleDocument;
using test::ExampleHit;
using test::FrameText;
using test::HasRole;
using test::HtmlDocument;
using test::ReaderState;
using test::ResultsState;
using test::SamplePage;

TEST(Render, StripsTerminalControlsButPreservesValidUtf8) {
  EXPECT_EQ(SafeText("hello\x1b[31m\r\n"), "hello?[31m??");
  EXPECT_EQ(SafeText("caf\xc3\xa9"), "caf\xc3\xa9");
  EXPECT_EQ(SafeText("\xf0\x9f\x99\x82"), "\xf0\x9f\x99\x82");
  EXPECT_EQ(SafeText("\xc2\x9b"), "?");
  EXPECT_EQ(SafeText("\xc0\xaf"), "??");
  EXPECT_EQ(SafeText("\xed\xa0\x80"), "???");
}

TEST(Render, WideAndNarrowFramesStayWithinTheirAsciiViewport) {
  Snapshot state;
  state.query = std::string(200, 'q');
  state.hits = {ExampleHit()};
  state.status = "1 result";
  for (int width : {40, 60, 100, 200}) {
    const auto frame = RenderLines(state, width, 20);
    ASSERT_EQ(frame.size(), 20u);
    for (const auto& row : frame)
      EXPECT_LE(TextColumns(row), static_cast<std::size_t>(width));
    EXPECT_TRUE(std::ranges::any_of(frame, [](const auto& row) {
      return row.find("1 RESULTS") != std::string::npos;
    }));
    EXPECT_EQ(frame.back().find("Ctrl+Q"), std::string::npos);
  }

  const auto tiny = RenderLines(state, 15, 3);
  EXPECT_EQ(tiny.size(), 3u);
  EXPECT_NE(tiny[0].find("Terminal"), std::string::npos);
}

TEST(Render, ManualSectionsAndReaderBodyRemainDistinct) {
  EXPECT_EQ(PageLabel({"id", "printf", "3p", "", ""}), "printf(3p)");
  Snapshot state;
  state.route = Route::kReader;
  state.document = ExampleDocument();
  auto lines = RenderLines(state, 80, 20);
  EXPECT_TRUE(std::ranges::any_of(lines,
    [](const auto& line) { return line.find("NAME") != std::string::npos; }));
  EXPECT_TRUE(std::ranges::any_of(lines, [](const auto& line) {
    return line.find("SYNOPSIS") != std::string::npos;
  }));
}
TEST(Presentation, WordsCrossStyleBoundariesAndOnlyExplicitBreaksEndLines) {
  const auto page =
    HtmlDocument("<div>Call <i>socket</i>() now.<br>Next <b>line</b>.</div>");
  const auto lines = RenderDocumentText(page, 8);
  ASSERT_GE(lines.size(), 5u);
  EXPECT_EQ(lines[0], "Call");
  EXPECT_EQ(lines[1], "socket()");
  EXPECT_EQ(lines[2], "now.");
  EXPECT_EQ(lines[3], "Next");
  EXPECT_EQ(lines[4], "line.");
}

TEST(Presentation, DefinitionListsAndNestedOrderedListsKeepTheirHierarchy) {
  const auto page = HtmlDocument(
    "<main><dl><dt><code>domain</code></dt><dd><p>Address family.</p></dd></dl>"
    "<ol start='3'><li>Install the package<ul><li>Keep the originals</li></ul>"
    "<p>Then continue.</p></li><li>Read a page</li></ol></main>");
  const auto lines = RenderDocumentText(page, 60);
  ASSERT_GE(lines.size(), 7u);
  EXPECT_EQ(lines[0], "domain");
  EXPECT_EQ(lines[1], "  Address family.");
  EXPECT_NE(std::ranges::find(lines, "3. Install the package"), lines.end());
  EXPECT_NE(std::ranges::find(lines, "   • Keep the originals"), lines.end());
  EXPECT_NE(std::ranges::find(lines, "   Then continue."), lines.end());
  EXPECT_NE(std::ranges::find(lines, "4. Read a page"), lines.end());
}

TEST(Presentation, TableColumnsFollowContentAndSpanningHeadersUseTheWholeRow) {
  const auto page = HtmlDocument(
    "<main><table><tr><th colspan='3'>Parameter requirements</th></tr>"
    "<tr><td>pos</td><td>-</td><td>iterator after which the content is "
    "inserted</td></tr>"
    "</table></main>");
  ASSERT_TRUE(page.content);
  ASSERT_EQ(page.content->blocks.size(), 1u);
  EXPECT_EQ(page.content->blocks[0].rows[0].column_spans[0], 3u);
  const auto lines = RenderDocumentText(page, 80);
  ASSERT_GE(lines.size(), 3u);
  EXPECT_NE(lines[0].find("Parameter requirements"), std::string::npos);
  EXPECT_NE(
    lines[2].find("│ pos │ - │ iterator after which the content is inserted"),
    std::string::npos);
  for (const auto& line : lines)
    EXPECT_LE(TextColumns(line), 80u);

  const auto narrow = RenderDocumentText(page, 30);
  EXPECT_NE(std::ranges::find(narrow, "pos"), narrow.end());
  EXPECT_EQ(std::ranges::find(narrow, "  -"), narrow.end());
  EXPECT_TRUE(std::ranges::any_of(narrow, [](const auto& line) {
    return line.starts_with("  iterator after which");
  }));
}

TEST(Presentation, NarrowCodeAndPreformattedLinesKeepAllSourceCharacters) {
  const std::string source_line =
    "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_TAIL";
  for (const auto tag : {"<pre>", "<pre><code>"}) {
    const auto page = HtmlDocument(
      std::string("<main>") + tag + source_line +
      (std::string_view(tag) == "<pre>" ? "</pre>" : "</code></pre>") +
      "</main>");
    const auto lines = RenderDocumentText(page, 20);
    std::string reconstructed;
    for (auto line : lines) {
      EXPECT_LE(TextColumns(line), 20u);
      if (line.starts_with("╭") || line.starts_with("╰") || line.empty())
        continue;
      if (line.starts_with("│ ")) {
        line = line.substr(std::string("│ ").size());
        line.resize(line.size() - std::string(" │").size());
        line.erase(line.find_last_not_of(' ') + 1);
      }
      reconstructed += line;
    }
    EXPECT_EQ(reconstructed, source_line);
  }

  const auto declaration =
    HtmlDocument("<pre>    using value_type = std::vector&lt;int&gt;;</pre>");
  const auto wrapped = RenderDocumentText(declaration, 24);
  EXPECT_NE(std::ranges::find(wrapped, "std::vector<int>;"), wrapped.end());
}

TEST(Presentation, OfflineCppDeclarationsKeepIndentationAndTheirAnnotations) {
  Document page;
  page.body =
    "<div id='mw-content-text'><table class='t-dcl-begin'><tr class='t-dcl'>"
    "<td><div><span class='mw-geshi source-cpp'>template&lt;<br>\n<p>"
    "&nbsp; &nbsp; class T<br>\n</p>&gt; class vector;</span></div></td>"
    "<td>(1)</td><td>(since C++11)</td></tr></table></div>";
  auto content = AdaptContent(
    {page.body, SourceFormat::kHtml, "cpp", SourceFamily::kCppReference});
  ASSERT_TRUE(content);
  page.content = *content;
  ASSERT_EQ(page.content->blocks.size(), 1);
  const auto& declaration = page.content->blocks.front();
  EXPECT_EQ(declaration.kind, BlockKind::kCode);
  EXPECT_EQ(declaration.annotation, "(1) (since C++11)");

  const auto lines = RenderDocumentText(page, 80);
  const auto indented = std::ranges::find_if(lines,
    [](const auto& line) { return line.find("class T") != std::string::npos; });
  ASSERT_NE(indented, lines.end());
  // Two border columns precede the four-column HTML indentation.
  EXPECT_EQ(TextColumns(indented->substr(0, indented->find("class T"))), 6u);
}

TEST(Presentation, CaptionsRowSpansAndGroupedHeadersKeepColumnAssociations) {
  const auto page = HtmlDocument(
    "<main><table><caption>Availability</caption><thead>"
    "<tr><th rowspan='2'>Name</th><th colspan='2'>Versions</th></tr>"
    "<tr><th>C17</th><th>C23</th></tr></thead><tbody>"
    "<tr><th rowspan='2' scope='row'>foo</th><td>yes</td><td>no</td></tr>"
    "<tr><td>same</td><td>added</td></tr></tbody></table></main>");
  const auto& table = page.content->blocks.front();
  EXPECT_EQ(InlineText(table.text), "Availability");
  EXPECT_EQ(table.rows[0].row_spans[0], 2u);
  EXPECT_FALSE(table.rows[2].header);
  EXPECT_TRUE(table.rows[2].cell_headers[0]);
  EXPECT_FALSE(table.rows[2].cell_headers[1]);

  const auto wide = RenderDocumentText(page, 80);
  EXPECT_EQ(wide.front(), "Availability");
  const auto header = std::ranges::find_if(wide,
    [](const auto& line) { return line.find("C17") != std::string::npos; });
  const auto continued = std::ranges::find_if(wide,
    [](const auto& line) { return line.find("same") != std::string::npos; });
  ASSERT_NE(header, wide.end());
  ASSERT_NE(continued, wide.end());
  EXPECT_NE(continued->find("↑"), std::string::npos);
  EXPECT_EQ(TextColumns(header->substr(0, header->find("C17"))),
    TextColumns(continued->substr(0, continued->find("same"))));
  EXPECT_EQ(TextColumns(header->substr(0, header->find("C23"))),
    TextColumns(continued->substr(0, continued->find("added"))));

  const auto narrow = RenderDocumentText(page, 12);
  for (const auto text : {"Name: foo", "Name: ↑", "C17: same", "C23: added"})
    EXPECT_NE(std::ranges::find(narrow, text), narrow.end());
  for (const auto& line : narrow)
    EXPECT_LE(TextColumns(line), 12u);
}

TEST(Presentation, OpenEndedRowSpansStopAtTheNextBodyGroup) {
  const auto page = HtmlDocument(
    "<main><table><tbody><tr><td rowspan='0'>A</td><td>one</td></tr>"
    "<tr><td>two</td></tr></tbody><tbody>"
    "<tr><td>B</td><td>three</td></tr></tbody></table></main>");
  const auto& rows = page.content->blocks.front().rows;
  EXPECT_NE(rows[0].group, rows[2].group);
  const auto lines = RenderDocumentText(page, 80);
  EXPECT_TRUE(std::ranges::any_of(lines, [](const auto& line) {
    return line.find("│ B │ three") != std::string::npos;
  }));
  EXPECT_TRUE(std::ranges::any_of(lines, [](const auto& line) {
    return line.find("│ ↑ │ two") != std::string::npos;
  }));
}

TEST(Presentation, DisclosuresAndFiguresKeepTheirSeparateContentBlocks) {
  const auto page = HtmlDocument(
    "<main><details><summary>Implementation notes</summary>"
    "<p>First paragraph.</p><pre>int value = 4;</pre></details>"
    "<figure><img alt='Container diagram'><figcaption>Node relationships"
    "</figcaption></figure><p>Following paragraph.</p></main>");
  const auto lines = RenderDocumentText(page, 80);
  for (const auto text : {"Implementation notes",
         "First paragraph.",
         "int value = 4;",
         "Container diagram",
         "Node relationships",
         "Following paragraph."})
    EXPECT_NE(std::ranges::find(lines, text), lines.end());
}

TEST(
  Presentation, ResultsHaveClearBoundariesAndActualNonduplicatedPagePreview) {
  const auto state = ResultsState();
  const auto frame = RenderFrame(state, 156, 48);
  const auto text = FrameText(frame);
  for (const auto* expected : {"2 RESULTS",
         "01",
         "02",
         "end of results",
         "PAGE PREVIEW",
         "SYNOPSIS",
         "insert_after"})
    EXPECT_NE(text.find(expected), std::string::npos) << expected;
  EXPECT_EQ(text.find(state.hits[0].description), std::string::npos);
  EXPECT_TRUE(HasRole(frame, StyleRole::kSelected));
  EXPECT_TRUE(HasRole(frame, StyleRole::kSelectedMeta));
  EXPECT_TRUE(HasRole(frame, StyleRole::kCode));
  EXPECT_TRUE(HasRole(frame, StyleRole::kHeading));
  EXPECT_FALSE(frame.cursor);
}

TEST(Presentation, ManualSectionsRemainVisibleInReaderAndPreviewTitles) {
  auto page = SamplePage("man/printf/3");
  page.manual_section = "3p";
  const auto reader = RenderFrame(ReaderState(page), 132, 42);
  EXPECT_NE(PlainLines(reader)[0].find("printf(3p)"), std::string::npos);
  Snapshot state;
  state.hits = {{page.id, page.title, page.manual_section, "", page.source}};
  state.preview = page;
  const auto preview = RenderFrame(state, 132, 42);
  // The first content row contains both the candidate and preview titles.
  const auto line = PlainLines(preview)[4];
  const auto first = line.find("printf(3p)");
  ASSERT_NE(first, std::string::npos);
  EXPECT_NE(line.find("printf(3p)", first + 1), std::string::npos);
}

TEST(Presentation, TerminalDefaultsReplaceThemeKeycapsAndPassiveSidebar) {
  const auto frame = RenderFrame(ResultsState(), 132, 42);
  EXPECT_FALSE(HasRole(frame, StyleRole::kKey));
  EXPECT_FALSE(HasRole(frame, StyleRole::kKeyLabel));
  const auto ansi = SerializeAnsi(frame);
  EXPECT_NE(ansi.find("39;49"), std::string::npos);
  EXPECT_NE(ansi.find(";7m"), std::string::npos);
  // Cursor row 38 uses ESC[38;1H; only SGR (ending in m) sets colors.
  EXPECT_FALSE(
    std::regex_search(ansi, std::regex("\x1b\\[[0-9;]*(38|48);[0-9;]*m")));
  EXPECT_EQ(FrameText(frame).find("Read page"), std::string::npos);
  EXPECT_EQ(FrameText(RenderFrame(ReaderState(SamplePage()), 156, 48))
              .find("ON THIS PAGE"),
    std::string::npos);
  EXPECT_EQ(SerializeHtml(frame).find("#0b1220"), std::string::npos);
}

TEST(Presentation, CodeUsesKeywordStringNumberAndCommentStyles) {
  auto page = HtmlDocument(
    "<main><pre><code>auto message = &quot;hello&quot;;\nint value = 42; // "
    "sample\n</code></pre></main>");
  const auto frame = RenderFrame(ReaderState(page), 100, 32);
  EXPECT_TRUE(HasRole(frame, StyleRole::kKeyword));
  EXPECT_TRUE(HasRole(frame, StyleRole::kString));
  EXPECT_TRUE(HasRole(frame, StyleRole::kNumber));
  EXPECT_TRUE(HasRole(frame, StyleRole::kComment));
}

TEST(Presentation, UnicodeCodeIdentifiersAreNotSplitIntoInvalidUtf8) {
  const auto page =
    HtmlDocument("<main><pre><code>int 变量 = 1;</code></pre></main>");
  EXPECT_NE(FrameText(RenderFrame(ReaderState(page), 100, 32)).find("变量"),
    std::string::npos);
}

TEST(Presentation, SmallResultViewDoesNotPretendTheRemainingResultsEnded) {
  auto state = ResultsState();
  while (state.hits.size() < 10)
    state.hits.push_back(state.hits.front());
  const auto frame = RenderFrame(state, 60, 14);
  EXPECT_NE(FrameText(frame).find("more below"), std::string::npos);
  EXPECT_EQ(FrameText(frame).find("end of results"), std::string::npos);
}

TEST(Presentation, LongProseWrapsAndRenderedScrollLimitsDoNotIssueQueries) {
  auto page = SamplePage();
  page.body = "DESCRIPTION\n";
  for (int n = 0; n < 150; ++n)
    page.body += "A deliberately long paragraph with useful documentation. ";
  page.body += "TAIL_MARKER";
  auto parsed = AdaptContent({page.body, SourceFormat::kPlainText, ""});
  ASSERT_TRUE(parsed);
  page.content = *parsed;
  Browser browser;
  browser.Dispatch(Search{"forward"});
  browser.Dispatch(SearchCompleted{1, {{page.id, page.title, "", "", ""}}});
  browser.Dispatch(ReadCompleted{browser.state().generation, page, true});
  browser.Dispatch(OpenSelected{});
  const auto first = RenderFrame(browser.state(), 80, 24);
  ASSERT_GT(first.reader_scroll_limit, 20u);
  const auto scroll =
    browser.Dispatch(Scroll{10000, first.reader_scroll_limit});
  EXPECT_FALSE(scroll.request);
  EXPECT_EQ(browser.state().scroll_line, first.reader_scroll_limit);
  const auto last = RenderFrame(browser.state(), 80, 24);
  EXPECT_NE(FrameText(last).find("TAIL_MARKER"), std::string::npos);
  EXPECT_FALSE(last.cursor);
  browser.Dispatch(Scroll{-1, 5});  // A resize tightened the layout bounds.
  EXPECT_EQ(browser.state().scroll_line, 4u);
}

TEST(
  Presentation, ResponsiveFramesRespectCellWidthsWithoutPersistentShortcuts) {
  for (const int width : {40, 60, 80, 100, 132, 240}) {
    for (const int height : {14, 20, 42, 70}) {
      const auto frame = RenderFrame(ResultsState(), width, height);
      ASSERT_EQ(frame.lines.size(), static_cast<std::size_t>(height));
      for (const auto& line : PlainLines(frame))
        EXPECT_LE(TextColumns(line), static_cast<std::size_t>(width));
      EXPECT_NE(FrameText(frame).find("Query:"), std::string::npos);
      EXPECT_FALSE(frame.cursor);
      EXPECT_FALSE(HasRole(frame, StyleRole::kKey));
    }
  }

  EXPECT_EQ(TextColumns("中文"), 4u);
  EXPECT_EQ(TextColumns("e\xcc\x81"), 1u);
}

TEST(Presentation, TablesWrapOrStackRatherThanDiscardingCells) {
  auto page = HtmlDocument(
    "<main><table><tr><th>Operation</th><th>Notes</th><th>Cost</"
    "th><th>Version</th></tr>"
    "<tr><td>VALUE_A</td><td>VALUE_B</td><td>VALUE_C</td><td>VALUE_D</td></"
    "tr></table></main>");
  const auto frame = RenderFrame(ReaderState(page), 40, 40);
  for (const auto* value : {"VALUE_A", "VALUE_B", "VALUE_C", "VALUE_D"})
    EXPECT_NE(FrameText(frame).find(value), std::string::npos);
}

TEST(Presentation, ExportsAreInertEscapedAndDoNotDecorateStoredSource) {
  auto page = HtmlDocument(
    "<main><p>&lt;script&gt;not executable&lt;/script&gt; "
    "\x1b[2J</p></main>");
  const auto original = page.body;
  const auto frame = RenderFrame(ReaderState(page), 100, 32);
  const auto html = SerializeHtml(frame);
  EXPECT_NE(html.find("&lt;script&gt;"), std::string::npos);
  EXPECT_EQ(html.find("<script>"), std::string::npos);
  EXPECT_NE(html.find("default-src 'none'"), std::string::npos);
  EXPECT_EQ(SerializeAnsi(frame).find("\x1b[2J"), std::string::npos);
  EXPECT_EQ(FrameText(frame).find('\x1b'), std::string::npos);
  EXPECT_EQ(page.body, original);
  EXPECT_EQ(html, SerializeHtml(frame));
}
TEST(InPageSearch, LiteralOccurrencesCrossStylesAndPreserveUtf8Source) {
  const std::string source =
    "<main><p>Alpha <b>al</b><i>pha</i> 中文 alpha.</p></main>";
  Document document{
    "test/find", "Find", "", "", source, "Fixture", SourceFormat::kHtml};
  auto content = AdaptContent({source, document.format, ""});
  ASSERT_TRUE(content);
  document.content = *content;
  const auto matches = FindDocumentText(document, 80, "ALPHA");
  ASSERT_EQ(matches.size(), 3u);
  EXPECT_EQ(matches[0].line, matches[1].line);
  EXPECT_LT(matches[0].byte, matches[1].byte);
  EXPECT_EQ(FindDocumentText(document, 80, "中文").size(), 1u);
  EXPECT_TRUE(FindDocumentText(document, 80, "[a-z]").empty());
  Snapshot state;
  state.route = Route::kReader;
  state.document = document;
  TerminalPrompt prompt;
  prompt.find_query = "alpha";
  prompt.current_match = matches[1];
  const auto frame = RenderFrame(state, 80, 24, prompt);
  int current_spans = 0;
  for (const auto& line : frame.lines)
    for (const auto& span : line)
      if (span.role == StyleRole::kCurrentMatch)
        ++current_spans;
  EXPECT_EQ(current_spans, 2);  // The active match crosses b/i spans.
  EXPECT_EQ(frame.find_matches, matches);
  EXPECT_EQ(document.body, source);
}

TEST(PromptView, UnicodeInputScrollsWithinBottomLineAndCursorIsOnlyForEditors) {
  Snapshot state;
  EXPECT_FALSE(RenderFrame(state, 40, 14).cursor);
  TerminalPrompt prompt;
  prompt.mode = InputMode::kCommand;
  for (int n = 0; n < 100; ++n)
    prompt.text += "中";
  const auto frame = RenderFrame(state, 40, 14, prompt);
  ASSERT_TRUE(frame.cursor);
  EXPECT_EQ(frame.cursor->row, 13);
  EXPECT_LT(frame.cursor->column, 39);
  for (const auto& line : PlainLines(frame))
    EXPECT_LE(TextColumns(line), 40u);
  EXPECT_EQ(PlainLines(frame).back().find('?'), std::string::npos);
  EXPECT_NE(PlainLines(frame).back().find("…"), std::string::npos);
}

TEST(NavigationView, LinksSurviveWrappingTablesCodeAndFindHighlighting) {
  auto document = test::HtmlDocument(
    "<main><h1>Fixture</h1><p><a href='other.html'><b>linked</b> words</a>"
    " and <a href='other.html'>again</a></p>"
    "<pre class='code'>int <a href='function.html'>function</a>();</pre>"
    "<table><tr><td><a href='table.html'>table reference</a></td>"
    "<td>value</td></tr></table></main>");
  auto state = test::ReaderState(document);
  TerminalPrompt prompt;
  prompt.find_query = "linked";
  prompt.selected_link = "other.html";
  for (const int width : {40, 80, 132}) {
    const auto frame = RenderFrame(state, width, 24, prompt);
    ASSERT_EQ(frame.reader_links.size(), 3u);
    EXPECT_EQ(frame.reader_links[0].target, "other.html");
    EXPECT_EQ(frame.reader_links[1].target, "function.html");
    EXPECT_EQ(frame.reader_links[2].target, "table.html");
    EXPECT_TRUE(test::HasRole(frame, StyleRole::kCurrentMatch));
    EXPECT_NE(
      test::FrameText(frame).find("Link: other.html"), std::string::npos);
    for (const auto& line : PlainLines(frame))
      EXPECT_LE(TextColumns(line), static_cast<std::size_t>(width));
  }
}

TEST(NavigationView, NamedAndNestedAnchorsTrackBlocksAfterLayout) {
  std::string source = "<main><h1>Fixture</h1>";
  for (int number = 0; number < 40; ++number)
    source += "<p>Previous paragraph.</p>";
  source +=
    "<a name='named'></a><h2><span id='nested'>Target section</span>"
    "</h2><p>Visible target content.</p>";
  for (int number = 0; number < 40; ++number)
    source += "<p>Following paragraph.</p>";
  source += "</main>";
  auto state = test::ReaderState(test::HtmlDocument(source));
  for (const std::string anchor : {"named", "nested"}) {
    state.document_anchor = anchor;
    const auto frame = RenderFrame(state, 80, 24);
    EXPECT_GT(frame.reader_offset, 50u);
    EXPECT_NE(PlainLines(frame)[2].find("Target section"), std::string::npos);
  }
  state.document_anchor = "missing";
  EXPECT_NE(test::FrameText(RenderFrame(state, 80, 24)).find("unavailable"),
    std::string::npos);
  state.document_anchor.clear();
  state.status = "Error: Missing offline page";
  EXPECT_NE(test::FrameText(RenderFrame(state, 80, 24)).find(state.status),
    std::string::npos);
}

TEST(NavigationView, TableFragmentsResolveToTheirRowInWideAndStackedLayouts) {
  std::string source = "<main><table><tr><th>Name</th><th>Details</th></tr>";
  for (int number = 0; number < 80; ++number)
    source += "<tr id='row" + std::to_string(number) + "'><td>Feature " +
              std::to_string(number) +
              "</td><td>Long feature description "
              "that requires wrapping in narrower views.</td></tr>";
  source += "</table></main>";
  auto state = test::ReaderState(test::HtmlDocument(source));
  state.document_anchor = "row40";
  for (const int width : {40, 80, 132}) {
    const auto frame = RenderFrame(state, width, 24);
    EXPECT_GT(frame.reader_offset, 30u);
    EXPECT_NE(PlainLines(frame)[2].find("Feature 40"), std::string::npos);
  }
}

}  // namespace
}  // namespace manbo
