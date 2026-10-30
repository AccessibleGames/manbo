#include "support/fixtures.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <ranges>

#include "core/browser.h"
#include "repositories/sample_repository.h"

namespace manbo::test {
SearchHit
ExampleHit() {
  return {"cpp/forward-list",
    "std::forward_list",
    "",
    "Singly linked container",
    "Samples"};
}

Document
ExampleDocument() {
  Document document{"cpp/forward-list",
    "std::forward_list",
    "",
    "Singly linked container",
    "NAME\n  forward_list\nSYNOPSIS\n  #include <forward_list>\n",
    "Samples"};
  auto content = AdaptContent({document.body, SourceFormat::kPlainText, "cpp"});
  EXPECT_TRUE(content);
  if (content)
    document.content = *content;
  return document;
}

Document
HtmlDocument(std::string source) {
  Document document{"fixture/html",
    "Fixture",
    "",
    "",
    std::move(source),
    "Self-authored fixture",
    SourceFormat::kHtml};
  auto parsed = AdaptContent({document.body, document.format, "cpp"});
  EXPECT_TRUE(parsed);
  if (parsed)
    document.content = *parsed;
  return document;
}

Document
SamplePage(std::string_view id) {
  SampleRepository repository;
  auto page = repository.Read(id, {});
  EXPECT_TRUE(page);
  return page ? *page : Document{};
}

std::string
FrameText(const StyledFrame& frame) {
  std::string text;
  for (const auto& line : PlainLines(frame))
    text += line + '\n';
  return text;
}

bool
HasRole(const StyledFrame& frame, StyleRole role) {
  return std::ranges::any_of(frame.lines, [&](const auto& line) {
    return std::ranges::any_of(
      line, [&](const auto& span) { return span.role == role; });
  });
}

Snapshot
ResultsState() {
  SampleRepository repository;
  Browser browser;
  browser.Dispatch(Search{"std::forward_list"});
  auto hits = repository.Search("std::forward_list", {});
  EXPECT_TRUE(hits);
  if (hits)
    browser.Dispatch(SearchCompleted{1, *hits});
  browser.Dispatch(
    ReadCompleted{browser.state().generation, SamplePage(), true});
  return browser.state();
}

Snapshot
ReaderState(Document document) {
  Snapshot state;
  state.route = Route::kReader;
  state.document = std::move(document);
  state.status = "Fixture";
  return state;
}

std::string
Bytes(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  return {
    std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

std::string
ContentText(const RichDocument& document) {
  std::string text;
  for (const auto& block : document.blocks) {
    text += InlineText(block.text) + '\n';
    for (const auto& row : block.rows)
      for (const auto& cell : row.cells)
        text += InlineText(cell) + '\n';
  }
  return text;
}

}  // namespace manbo::test
