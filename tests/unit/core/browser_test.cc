#include "core/browser.h"

#include <gtest/gtest.h>

#include "repositories/sample_repository.h"
#include "support/fixtures.h"

namespace manbo {
namespace {
using test::ExampleDocument;
using test::ExampleHit;
using test::SamplePage;

TEST(Browser, SearchPromptDefersWorkAndPreservesStaleReplyProtection) {
  Browser browser;
  browser.Dispatch(Search{"old"});
  const auto home = browser.Dispatch(ShowSearch{});
  EXPECT_TRUE(home.cancel_work);
  EXPECT_FALSE(home.request);
  EXPECT_TRUE(browser.state().search_home);
  EXPECT_FALSE(browser.state().pending);

  const auto search = browser.Dispatch(Search{""});
  ASSERT_TRUE(search.request);
  EXPECT_FALSE(browser.state().search_home);
  EXPECT_EQ(GetRequestId(*search.request), 3u);
  EXPECT_FALSE(
    browser.Dispatch(SearchCompleted{1, {ExampleHit()}}).visible_change);
}

TEST(Browser, SearchCommitsStateBeforeReturningEffect) {
  Browser browser;
  const auto transition = browser.Dispatch(Search{"forward list"});
  EXPECT_TRUE(transition.visible_change);
  EXPECT_TRUE(browser.state().pending);
  EXPECT_EQ(browser.state().query, "forward list");
  ASSERT_TRUE(transition.request);
  EXPECT_EQ(std::get<SearchRequest>(*transition.request).id,
    browser.state().generation);
}

TEST(Browser, StaleRepliesAndDuplicateCompletionsAreIgnored) {
  Browser browser;
  browser.Dispatch(Search{"old"});
  browser.Dispatch(Search{"new"});
  EXPECT_FALSE(
    browser.Dispatch(SearchCompleted{1, {ExampleHit()}}).visible_change);
  EXPECT_TRUE(browser.state().pending);
  EXPECT_TRUE(
    browser.Dispatch(SearchCompleted{2, {ExampleHit()}}).visible_change);
  EXPECT_FALSE(browser.Dispatch(SearchCompleted{2, {}}).visible_change);
  EXPECT_EQ(browser.state().hits.size(), 1u);
}

TEST(Browser, SelectionClampsAndNoOpInputDoesNotRedrawOrQuery) {
  Browser browser;
  browser.Dispatch(Search{""});
  browser.Dispatch(SearchCompleted{1, {ExampleHit(), ExampleHit()}});
  EXPECT_FALSE(browser.Dispatch(Select{-1}).visible_change);
  EXPECT_FALSE(browser.Dispatch(Search{""}).visible_change);
  auto moved = browser.Dispatch(Select{1000});
  EXPECT_TRUE(moved.visible_change);
  ASSERT_TRUE(moved.request);
  EXPECT_TRUE(std::get<ReadRequest>(*moved.request).preview);
  EXPECT_EQ(browser.state().selection, 1u);
  EXPECT_FALSE(browser.Dispatch(Select{1}).visible_change);
}

TEST(Browser, ReadingAndBackRejectLatePageReplies) {
  Browser browser;
  browser.Dispatch(Search{"forward"});
  browser.Dispatch(SearchCompleted{1, {ExampleHit()}});
  auto open = browser.Dispatch(OpenSelected{});
  ASSERT_TRUE(open.request);
  EXPECT_EQ(browser.state().route, Route::kReader);
  EXPECT_EQ(std::get<ReadRequest>(*open.request).page_id, "cpp/forward-list");
  auto back = browser.Dispatch(Back{});
  EXPECT_TRUE(back.cancel_work);
  ASSERT_TRUE(back.request);
  EXPECT_TRUE(std::get<ReadRequest>(*back.request).preview);
  EXPECT_EQ(browser.state().route, Route::kResults);
  EXPECT_FALSE(
    browser.Dispatch(ReadCompleted{2, ExampleDocument()}).visible_change);
  EXPECT_FALSE(browser.state().document);
}

TEST(Browser, ScrollAndResizeDoNotIssueDataRequests) {
  Browser browser;
  browser.Dispatch(Search{"forward"});
  browser.Dispatch(SearchCompleted{1, {ExampleHit()}});
  browser.Dispatch(OpenSelected{});
  browser.Dispatch(
    ReadCompleted{browser.state().generation, ExampleDocument()});
  EXPECT_FALSE(browser.Dispatch(Scroll{-1}).visible_change);
  auto scroll = browser.Dispatch(Scroll{1000});
  EXPECT_TRUE(scroll.visible_change);
  EXPECT_FALSE(scroll.request);
  EXPECT_LE(browser.state().scroll_line, 4u);
  auto refresh = browser.Dispatch(Refresh{});
  EXPECT_TRUE(refresh.visible_change);
  EXPECT_FALSE(refresh.request);
}

TEST(Browser, MatchingErrorsEndPendingWorkAndQuitIsFinal) {
  Browser browser;
  browser.Dispatch(Search{"forward"});
  browser.Dispatch(JobFailed{1, {ErrorCode::kBackend, "fixture failure"}});
  EXPECT_FALSE(browser.state().pending);
  EXPECT_EQ(browser.state().status, "Error: fixture failure");
  auto quit = browser.Dispatch(Quit{});
  EXPECT_TRUE(quit.cancel_work);
  EXPECT_FALSE(browser.state().running);
  EXPECT_FALSE(browser.Dispatch(Search{"ignored"}).request);
}
TEST(Preview, ResultsRequestARealPageAndOpeningItsCacheNeedsNoNewRead) {
  Browser browser;
  browser.Dispatch(Search{"forward"});
  const auto page = SamplePage();
  auto result = browser.Dispatch(SearchCompleted{
    1, {{page.id, page.title, "", page.description, page.source}}});
  EXPECT_FALSE(browser.state().pending);
  EXPECT_TRUE(browser.state().preview_pending);
  ASSERT_TRUE(result.request);
  EXPECT_TRUE(std::get<ReadRequest>(*result.request).preview);
  browser.Dispatch(ReadCompleted{browser.state().generation, page, true});
  EXPECT_FALSE(browser.state().preview_pending);
  ASSERT_TRUE(browser.state().preview);
  EXPECT_FALSE(
    browser.Dispatch(ReadCompleted{browser.state().generation, page, true})
      .visible_change);
  const auto open = browser.Dispatch(OpenSelected{});
  EXPECT_TRUE(open.visible_change);
  EXPECT_FALSE(open.request);
  EXPECT_FALSE(browser.state().pending);
  ASSERT_TRUE(browser.state().document);
  EXPECT_EQ(browser.state().document->content, page.content);
  EXPECT_FALSE(browser.Dispatch(Back{}).request);
}

TEST(Preview, RapidSelectionRejectsOldRepliesAndNewQueriesInvalidatePreviews) {
  SampleRepository repository;
  auto hits = repository.Search("std::forward_list", {});
  ASSERT_TRUE(hits);
  Browser browser;
  browser.Dispatch(Search{"std::forward_list"});
  browser.Dispatch(SearchCompleted{1, *hits});
  const auto old = browser.state().generation;
  auto selected = browser.Dispatch(Select{1});
  ASSERT_TRUE(selected.request);
  const auto latest = browser.state().generation;
  EXPECT_FALSE(
    browser.Dispatch(ReadCompleted{old, SamplePage(), true}).visible_change);
  EXPECT_TRUE(browser.state().preview_pending);
  EXPECT_TRUE(browser
      .Dispatch(ReadCompleted{latest, SamplePage("cpp/insert-after"), true})
      .visible_change);
  EXPECT_EQ(browser.state().preview->id, "cpp/insert-after");
  browser.Dispatch(Search{"vector"});
  EXPECT_FALSE(browser.state().preview);
  EXPECT_FALSE(browser
      .Dispatch(ReadCompleted{latest, SamplePage("cpp/insert-after"), true})
      .visible_change);
}

TEST(Preview, EnterWhilePreviewLoadsSupersedesItWithoutAcceptingLateReplies) {
  Browser browser;
  const auto page = SamplePage();
  browser.Dispatch(Search{"forward"});
  browser.Dispatch(SearchCompleted{1, {{page.id, page.title, "", "", ""}}});
  const auto preview_id = browser.state().generation;
  const auto open = browser.Dispatch(OpenSelected{});
  ASSERT_TRUE(open.request);
  EXPECT_FALSE(std::get<ReadRequest>(*open.request).preview);
  EXPECT_GT(browser.state().generation, preview_id);
  EXPECT_FALSE(
    browser.Dispatch(ReadCompleted{preview_id, page, true}).visible_change);
  EXPECT_TRUE(browser.state().pending);
  EXPECT_FALSE(browser.state().document);
  browser.Dispatch(ReadCompleted{browser.state().generation, page});
  EXPECT_FALSE(browser.state().pending);
  ASSERT_TRUE(browser.state().document);
  EXPECT_EQ(browser.state().document->id, page.id);
}

TEST(Preview,
  WrongPageIdentityEndsLoadingWithAnErrorRatherThanShowingTheWrongPage) {
  Browser browser;
  const auto page = SamplePage();
  const auto wrong = SamplePage("man/printf/3");
  browser.Dispatch(Search{"forward"});
  browser.Dispatch(SearchCompleted{1, {{page.id, page.title, "", "", ""}}});
  EXPECT_TRUE(
    browser.Dispatch(ReadCompleted{browser.state().generation, wrong, true})
      .visible_change);
  EXPECT_FALSE(browser.state().preview_pending);
  EXPECT_FALSE(browser.state().preview);
  EXPECT_FALSE(browser.state().preview_error.empty());
  browser.Dispatch(OpenSelected{});
  browser.Dispatch(ReadCompleted{browser.state().generation, wrong});
  EXPECT_FALSE(browser.state().pending);
  EXPECT_FALSE(browser.state().document);
  EXPECT_TRUE(browser.state().status.starts_with("Error:"));
}

TEST(Preview, PreviewErrorsDoNotEraseResultsOrPreventExplicitOpen) {
  Browser browser;
  auto page = SamplePage();
  browser.Dispatch(Search{"forward"});
  browser.Dispatch(SearchCompleted{
    1, {{page.id, page.title, "", page.description, page.source}}});
  browser.Dispatch(JobFailed{browser.state().generation,
    {ErrorCode::kBackend, "Fixture preview failure"}});
  EXPECT_FALSE(browser.state().preview_pending);
  EXPECT_EQ(browser.state().hits.size(), 1u);
  EXPECT_FALSE(browser.state().preview_error.empty());
  auto open = browser.Dispatch(OpenSelected{});
  ASSERT_TRUE(open.request);
  EXPECT_FALSE(std::get<ReadRequest>(*open.request).preview);
}
TEST(
  DirectRead, OpeningReferencesRejectsLateOrWrongPagesAndKeepsResultsOnBack) {
  Browser browser;
  SampleRepository repository;
  const auto page = *repository.Read("printf(3)", {});
  auto start = browser.Dispatch(OpenPage{"printf(3)"});
  ASSERT_TRUE(start.request);
  EXPECT_TRUE(start.visible_change);
  EXPECT_EQ(std::get<ReadRequest>(*start.request).page_id, "printf(3)");
  EXPECT_TRUE(browser.state().pending);
  browser.Dispatch(ReadCompleted{browser.state().generation, page});
  ASSERT_TRUE(browser.state().document);
  const auto seek = browser.Dispatch(ScrollTo{100, 2});
  EXPECT_EQ(browser.state().scroll_line, 2u);
  EXPECT_TRUE(seek.visible_change);
  EXPECT_FALSE(seek.request);
  browser.Dispatch(Back{});
  browser.Dispatch(Search{"std::forward_list"});
  const auto hits = *repository.Search("std::forward_list", {});
  browser.Dispatch(SearchCompleted{browser.state().generation, hits});
  browser.Dispatch(ReadCompleted{
    browser.state().generation, *repository.Read(hits.front().id, {}), true});
  const auto old_generation = browser.state().generation;
  browser.Dispatch(OpenPage{"printf(3)"});
  EXPECT_FALSE(
    browser.Dispatch(ReadCompleted{old_generation, page}).visible_change);
  browser.Dispatch(ReadCompleted{browser.state().generation, page});
  auto back = browser.Dispatch(Back{});
  EXPECT_FALSE(back.request);
  EXPECT_EQ(browser.state().hits.front().id, hits.front().id);
  EXPECT_EQ(browser.state().preview->id, hits.front().id);
  browser.Dispatch(OpenPage{"printf(3)"});
  browser.Dispatch(ReadCompleted{
    browser.state().generation, *repository.Read("socket(2)", {})});
  EXPECT_FALSE(browser.state().document);
  EXPECT_TRUE(browser.state().status.starts_with("Error:"));
}

TEST(Navigation, LinksRestorePagesAndScrollWithoutBackendRequests) {
  Browser browser;
  const auto first = ExampleDocument();
  const auto second = SamplePage("man/printf/3");
  browser.Dispatch(OpenPage{first.id});
  browser.Dispatch(ReadCompleted{browser.state().generation, first});
  browser.Dispatch(ScrollTo{37, 100});

  const auto follow = browser.Dispatch(FollowLink{"other.html#example"});
  ASSERT_TRUE(follow.request);
  const auto& request = std::get<LinkRequest>(*follow.request);
  EXPECT_EQ(request.page_id, first.id);
  EXPECT_EQ(request.target, "other.html#example");
  EXPECT_EQ(browser.state().document->id, first.id);
  EXPECT_TRUE(browser.state().pending);
  browser.Dispatch(LinkCompleted{request.id, {second, "example"}});
  EXPECT_EQ(browser.state().document->id, second.id);
  EXPECT_EQ(browser.state().document_anchor, "example");
  browser.Dispatch(Scroll{1, 100, 50});
  EXPECT_EQ(browser.state().scroll_line, 51u);
  EXPECT_TRUE(browser.state().document_anchor.empty());

  const auto back = browser.Dispatch(Back{});
  EXPECT_TRUE(back.cancel_work);
  EXPECT_FALSE(back.request);
  EXPECT_EQ(browser.state().document->id, first.id);
  EXPECT_EQ(browser.state().scroll_line, 37u);
  browser.Dispatch(Back{});
  EXPECT_TRUE(browser.state().search_home);
}

TEST(Navigation, FailedAndCancelledLinksPreserveTheCurrentPage) {
  Browser browser;
  const auto page = ExampleDocument();
  browser.Dispatch(OpenPage{page.id});
  browser.Dispatch(ReadCompleted{browser.state().generation, page});
  browser.Dispatch(FollowLink{"missing.html"});
  browser.Dispatch(JobFailed{browser.state().generation,
    {ErrorCode::kNotFound, "Missing offline reference"}});
  EXPECT_EQ(browser.state().document->id, page.id);
  EXPECT_FALSE(browser.state().pending);
  EXPECT_TRUE(browser.state().status.starts_with("Error:"));

  browser.Dispatch(FollowLink{"later.html"});
  const auto cancelled = browser.state().generation;
  const auto back = browser.Dispatch(Back{});
  EXPECT_TRUE(back.cancel_work);
  EXPECT_EQ(browser.state().route, Route::kReader);
  EXPECT_FALSE(browser.state().pending);
  EXPECT_FALSE(
    browser.Dispatch(LinkCompleted{cancelled, {page, "late"}}).visible_change);
  browser.Dispatch(Back{});
  EXPECT_TRUE(browser.state().search_home);
}

TEST(Navigation, DirectCommandsShareBoundedHistoryAndSearchClearsIt) {
  Browser browser;
  auto page = ExampleDocument();
  for (int number = 0; number < 20; ++number) {
    page.id = "page/" + std::to_string(number);
    browser.Dispatch(OpenPage{page.id});
    browser.Dispatch(ReadCompleted{browser.state().generation, page});
  }
  for (int number = 18; number >= 3; --number) {
    const auto back = browser.Dispatch(Back{});
    EXPECT_FALSE(back.request);
    ASSERT_TRUE(browser.state().document);
    EXPECT_EQ(browser.state().document->id, "page/" + std::to_string(number));
  }
  browser.Dispatch(Back{});
  EXPECT_TRUE(browser.state().search_home);
  browser.Dispatch(OpenPage{page.id});
  browser.Dispatch(ReadCompleted{browser.state().generation, page});
  browser.Dispatch(Search{"fresh"});
  browser.Dispatch(OpenPage{page.id});
  browser.Dispatch(ReadCompleted{browser.state().generation, page});
  browser.Dispatch(Back{});
  EXPECT_EQ(browser.state().route, Route::kResults);
  EXPECT_FALSE(browser.state().document);
}

}  // namespace
}  // namespace manbo
