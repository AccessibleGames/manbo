#include "runtime/session.h"

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <stdexcept>
#include <thread>

#include "support/fixtures.h"

namespace manbo {
namespace {
using test::ExampleDocument;
using test::ExampleHit;

class ScriptEvents final : public IEventSource {
 public:
  explicit ScriptEvents(std::vector<Event> events)
      : events_(std::move(events)) {
  }

  Result<std::optional<Event>>
  Next() override {
    if (fail)
      return tl::make_unexpected(Error{ErrorCode::kPlatform, "Input failed"});
    if (position_ == events_.size())
      return std::optional<Event>{};
    return std::optional<Event>{events_[position_++]};
  }
  bool fail = false;

 private:
  std::vector<Event> events_;
  std::size_t position_ = 0;
};

class RecordingRenderer final : public IRenderer {
 public:
  explicit RecordingRenderer(std::vector<std::string>& log) : log_(log) {
  }

  Result<void>
  Render(const Snapshot& snapshot) override {
    EXPECT_EQ(std::this_thread::get_id(), owner_);
    if (throw_render)
      throw std::runtime_error("Render exception");
    states.push_back(snapshot);
    log_.push_back(snapshot.pending ? "render:pending" : "render:ready");
    return {};
  }

  Result<void>
  Flush() override {
    log_.push_back("flush");
    if (fail_flush)
      return tl::make_unexpected(Error{ErrorCode::kPlatform, "Flush failed"});
    return {};
  }
  std::vector<Snapshot> states;
  bool fail_flush = false;
  bool throw_render = false;

 private:
  std::vector<std::string>& log_;
  const std::thread::id owner_ = std::this_thread::get_id();
};

class RecordingJobs final : public IJobService {
 public:
  explicit RecordingJobs(std::vector<std::string>& log) : log_(log) {
  }

  Result<void>
  Submit(Request request) override {
    log_.push_back(fmt::format("dispatch:{}", GetRequestId(request)));
    if (fail)
      return tl::make_unexpected(Error{ErrorCode::kClosed, "Jobs closed"});
    requests.push_back(std::move(request));
    return {};
  }

  void
  Cancel() noexcept override {
    log_.push_back("cancel");
  }

  void
  Shutdown() noexcept override {
    log_.push_back("shutdown");
  }
  std::vector<Request> requests;
  bool fail = false;

 private:
  std::vector<std::string>& log_;
};

TEST(Session, SearchPromptRendersWithoutSubmittingBackendWork) {
  std::vector<std::string> log;
  ScriptEvents events({Quit{}});
  RecordingRenderer renderer(log);
  RecordingJobs jobs(log);
  Session session(events, renderer, jobs);
  auto result = session.RunInitial(ShowSearch{});
  ASSERT_TRUE(result);
  EXPECT_EQ(result->requests, 0u);
  EXPECT_EQ(result->flushes, 1u);
  EXPECT_TRUE(jobs.requests.empty());
  ASSERT_EQ(renderer.states.size(), 1u);
  EXPECT_TRUE(renderer.states.front().search_home);
  EXPECT_FALSE(renderer.states.front().pending);
}

TEST(Session, FlushesVisibleStateBeforeDispatchAndNavigationNeedsNoExtraQuery) {
  std::vector<std::string> log;
  ScriptEvents events({SearchCompleted{1, {ExampleHit()}},
    ReadCompleted{2, ExampleDocument(), true},
    OpenSelected{},
    Back{},
    Quit{}});
  RecordingRenderer renderer(log);
  RecordingJobs jobs(log);
  Session session(events, renderer, jobs);
  auto result = session.Run("forward");
  ASSERT_TRUE(result);
  EXPECT_EQ(result->renders, 5u);
  EXPECT_EQ(result->flushes, 5u);
  EXPECT_EQ(result->requests, 2u);
  EXPECT_EQ(result->events, 5u);
  const std::vector<std::string> expected{"render:pending",
    "flush",
    "dispatch:1",
    "render:ready",
    "flush",
    "dispatch:2",
    "render:ready",
    "flush",
    "render:ready",
    "flush",
    "render:ready",
    "flush",
    "cancel",
    "cancel",
    "shutdown"};
  EXPECT_EQ(log, expected);
  ASSERT_EQ(renderer.states.size(), 5u);
  EXPECT_EQ(renderer.states.front().query, "forward");
  EXPECT_TRUE(renderer.states.front().pending);
  EXPECT_EQ(renderer.states.back().route, Route::kResults);
  EXPECT_FALSE(session.Run("again"));
}

TEST(Session, SelectionIsRenderedAndFlushedBeforeItsNewPreviewRead) {
  std::vector<std::string> log;
  auto second = ExampleHit();
  second.id = "cpp/vector";
  second.title = "std::vector";
  ScriptEvents events({SearchCompleted{1, {ExampleHit(), second}},
    Select{1},
    ReadCompleted{2, ExampleDocument(), true},
    Quit{}});
  RecordingRenderer renderer(log);
  RecordingJobs jobs(log);
  Session session(events, renderer, jobs);
  const auto result = session.Run("std");
  ASSERT_TRUE(result);
  EXPECT_EQ(result->requests, 3u);
  EXPECT_EQ(log,
    (std::vector<std::string>{"render:pending",
      "flush",
      "dispatch:1",
      "render:ready",
      "flush",
      "dispatch:2",
      "render:ready",
      "flush",
      "dispatch:3",
      "cancel",
      "shutdown"}));
  ASSERT_EQ(renderer.states.size(), 3u);
  EXPECT_EQ(renderer.states.back().selection, 1u);
  EXPECT_TRUE(renderer.states.back().preview_pending);
  EXPECT_FALSE(renderer.states.back().preview);
  const auto& read = std::get<ReadRequest>(jobs.requests.back());
  EXPECT_EQ(read.page_id, second.id);
  EXPECT_TRUE(read.preview);
}

TEST(Session, ReferenceFeedbackFlushesBeforeDispatchAndBackNeedsNoJob) {
  std::vector<std::string> log;
  const auto page = ExampleDocument();
  auto linked = page;
  linked.id = "linked/page";
  ScriptEvents events({ReadCompleted{1, page},
    FollowLink{"linked.html#section"},
    LinkCompleted{2, {linked, "section"}},
    Back{},
    Quit{}});
  RecordingRenderer renderer(log);
  RecordingJobs jobs(log);
  Session session(events, renderer, jobs);
  const auto result = session.RunInitial(OpenPage{page.id});
  ASSERT_TRUE(result);
  EXPECT_EQ(result->requests, 2u);
  ASSERT_EQ(jobs.requests.size(), 2u);
  EXPECT_TRUE(std::holds_alternative<LinkRequest>(jobs.requests.back()));
  const auto dispatch = std::ranges::find(log, "dispatch:2");
  ASSERT_GE(dispatch - log.begin(), 2);
  EXPECT_EQ(*(dispatch - 2), "render:pending");
  EXPECT_EQ(*(dispatch - 1), "flush");
  ASSERT_EQ(renderer.states.size(), 5u);
  EXPECT_EQ(renderer.states.back().document->id, page.id);
}

TEST(Session, AFlushFailurePreventsQueryDispatch) {
  std::vector<std::string> log;
  ScriptEvents events({});
  RecordingRenderer renderer(log);
  renderer.fail_flush = true;
  RecordingJobs jobs(log);
  Session session(events, renderer, jobs);
  auto result = session.Run("forward");
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, ErrorCode::kPlatform);
  EXPECT_TRUE(jobs.requests.empty());
  EXPECT_EQ(
    log, (std::vector<std::string>{"render:pending", "flush", "shutdown"}));
}

TEST(Session, InputFailureStillShutsDownJobs) {
  std::vector<std::string> log;
  ScriptEvents events({});
  events.fail = true;
  RecordingRenderer renderer(log);
  RecordingJobs jobs(log);
  Session session(events, renderer, jobs);
  auto result = session.Run();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().message, "Input failed");
  EXPECT_EQ(log.back(), "shutdown");
}

TEST(Session, CoroutineExceptionsBecomeErrorsAndShutDownJobs) {
  std::vector<std::string> log;
  ScriptEvents events({Quit{}});
  RecordingRenderer renderer(log);
  renderer.throw_render = true;
  RecordingJobs jobs(log);
  Session session(events, renderer, jobs);
  auto result = session.Run();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, ErrorCode::kBackend);
  EXPECT_EQ(result.error().message, "Render exception");
  EXPECT_EQ(log.back(), "shutdown");
  EXPECT_TRUE(jobs.requests.empty());
}

TEST(Session, AdmissionErrorsBecomeVisibleStateRatherThanAnException) {
  std::vector<std::string> log;
  ScriptEvents events({Quit{}});
  RecordingRenderer renderer(log);
  RecordingJobs jobs(log);
  jobs.fail = true;
  Session session(events, renderer, jobs);
  auto result = session.Run();
  ASSERT_TRUE(result);
  ASSERT_EQ(renderer.states.size(), 2u);
  EXPECT_FALSE(renderer.states.back().pending);
  EXPECT_EQ(renderer.states.back().status, "Error: Jobs closed");
}

}  // namespace
}  // namespace manbo
