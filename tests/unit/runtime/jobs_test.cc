#include "runtime/jobs.h"

#include <gtest/gtest.h>

#include <atomic>
#include <future>
#include <semaphore>
#include <thread>

#include "repositories/sample_repository.h"
#include "support/fixtures.h"

namespace manbo {
namespace {
using namespace std::chrono_literals;

using test::ExampleDocument;
using test::ExampleHit;

std::optional<Event>
ReceiveWithin(EventChannel& channel) {
  std::promise<std::optional<Event>> promise;
  auto future = promise.get_future();
  std::jthread receiver(
    [&](std::stop_token stop) { promise.set_value(channel.Receive(stop)); });
  if (future.wait_for(2s) != std::future_status::ready)
    receiver.request_stop();
  return future.get();
}

class GatedRepository final : public IDocumentRepository {
 public:
  Result<std::vector<SearchHit>>
  Search(std::string_view query, std::stop_token stop) override {
    ++calls;
    if (query == "hold") {
      entered.release();
      release.acquire();
    }
    if (stop.stop_requested())
      return tl::make_unexpected(Error{ErrorCode::kCancelled, "Cancelled"});
    return std::vector<SearchHit>{ExampleHit()};
  }

  Result<Document>
  Read(std::string_view, std::stop_token) override {
    return ExampleDocument();
  }
  std::atomic<int> calls = 0;
  std::binary_semaphore entered{0};
  std::binary_semaphore release{0};
};

TEST(Jobs, InlineAndWorkerProfilesUseTheSameProtocol) {
  for (const std::size_t workers : {0u, 1u, 3u}) {
    auto channel = EventChannel::Create();
    ASSERT_TRUE(channel);
    SampleRepository repository;
    auto jobs = MakeJobService(repository, **channel, workers);
    ASSERT_TRUE(jobs);
    ASSERT_TRUE((*jobs)->Submit(SearchRequest{7, "std::forward_list"}));
    auto event = ReceiveWithin(**channel);
    ASSERT_TRUE(event);
    ASSERT_TRUE(std::holds_alternative<SearchCompleted>(*event));
    EXPECT_EQ(std::get<SearchCompleted>(*event).id, 7u);
    EXPECT_FALSE(std::get<SearchCompleted>(*event).hits.empty());
    (*jobs)->Shutdown();
    EXPECT_FALSE((*jobs)->Submit(SearchRequest{8, "vector"}));
  }
}

TEST(Jobs, LinkRequestsUseTheSameSuccessAndFailureProtocol) {
  for (const std::size_t workers : {0u, 1u, 3u}) {
    auto channel = EventChannel::Create();
    ASSERT_TRUE(channel);
    SampleRepository repository;
    auto jobs = MakeJobService(repository, **channel, workers);
    ASSERT_TRUE(jobs);
    ASSERT_TRUE((*jobs)->Submit(LinkRequest{11, "origin", "printf(3)"}));
    auto reply = ReceiveWithin(**channel);
    ASSERT_TRUE(reply);
    ASSERT_TRUE(std::holds_alternative<LinkCompleted>(*reply));
    const auto& page = std::get<LinkCompleted>(*reply);
    EXPECT_EQ(page.id, 11u);
    EXPECT_EQ(page.page.document.title, "printf");
    EXPECT_TRUE(page.page.anchor.empty());

    ASSERT_TRUE((*jobs)->Submit(LinkRequest{12, "origin", "missing"}));
    reply = ReceiveWithin(**channel);
    ASSERT_TRUE(reply);
    ASSERT_TRUE(std::holds_alternative<JobFailed>(*reply));
    EXPECT_EQ(std::get<JobFailed>(*reply).id, 12u);
    (*jobs)->Shutdown();
  }
}

TEST(Jobs, LatestPendingQueryWinsWhileOneWorkerIsBusy) {
  auto channel = EventChannel::Create(2);
  ASSERT_TRUE(channel);
  GatedRepository repository;
  auto jobs = MakeJobService(repository, **channel, 1);
  ASSERT_TRUE(jobs);
  ASSERT_TRUE((*jobs)->Submit(SearchRequest{1, "hold"}));
  repository.entered.acquire();
  bool accepted = true;
  for (RequestId id = 2; id <= 100; ++id)
    accepted = accepted && bool((*jobs)->Submit(SearchRequest{id, "latest"}));
  EXPECT_TRUE(accepted);
  EXPECT_EQ(repository.calls.load(), 1);
  repository.release.release();
  auto reply = ReceiveWithin(**channel);
  (*jobs)->Shutdown();
  ASSERT_TRUE(reply);
  EXPECT_EQ(std::get<SearchCompleted>(*reply).id, 100u);
  EXPECT_EQ(repository.calls.load(), 2);
  EXPECT_FALSE((*channel)->TryReceive());
}

TEST(Jobs, BoundedPoolCanProgressWhileObsoleteWorkIsStillBlocking) {
  auto channel = EventChannel::Create();
  ASSERT_TRUE(channel);
  GatedRepository repository;
  auto jobs = MakeJobService(repository, **channel, 2);
  ASSERT_TRUE(jobs);
  ASSERT_TRUE((*jobs)->Submit(SearchRequest{1, "hold"}));
  repository.entered.acquire();
  const bool accepted = bool((*jobs)->Submit(SearchRequest{2, "new"}));
  auto reply = ReceiveWithin(**channel);
  repository.release.release();  // Release even if an expectation fails.
  (*jobs)->Shutdown();
  EXPECT_TRUE(accepted);
  ASSERT_TRUE(reply);
  EXPECT_EQ(std::get<SearchCompleted>(*reply).id, 2u);
  EXPECT_EQ(repository.calls.load(), 2);
}

TEST(Jobs, CancelSuppressesAnObsoleteCompletion) {
  auto channel = EventChannel::Create();
  ASSERT_TRUE(channel);
  GatedRepository repository;
  auto jobs = MakeJobService(repository, **channel, 1);
  ASSERT_TRUE(jobs);
  ASSERT_TRUE((*jobs)->Submit(SearchRequest{1, "hold"}));
  repository.entered.acquire();
  (*jobs)->Cancel();
  repository.release.release();
  (*jobs)->Shutdown();
  EXPECT_FALSE((*channel)->TryReceive());
}

TEST(Jobs, ShutdownDoesNotNeedUIToDrainAFullReplyQueue) {
  auto channel = EventChannel::Create(1);
  ASSERT_TRUE(channel);
  EXPECT_TRUE((*channel)->TrySend(SearchCompleted{999, {}}));
  SampleRepository repository;
  auto jobs = MakeJobService(repository, **channel, 1);
  ASSERT_TRUE(jobs);
  EXPECT_TRUE((*jobs)->Submit(SearchRequest{1, "vector"}));
  const auto start = std::chrono::steady_clock::now();
  (*jobs)->Shutdown();
  (*jobs)->Shutdown();
  EXPECT_LT(std::chrono::steady_clock::now() - start, 2s);
}

TEST(Jobs, TypedFailuresReachTheConsumerAndInvalidProfilesAreRejected) {
  auto channel = EventChannel::Create();
  ASSERT_TRUE(channel);
  SampleRepository repository;
  EXPECT_FALSE(MakeJobService(repository, **channel, 17));
  auto jobs = MakeJobService(repository, **channel, 0);
  ASSERT_TRUE(jobs);
  EXPECT_TRUE((*jobs)->Submit(ReadRequest{5, "printf"}));
  auto reply = (*channel)->TryReceive();
  ASSERT_TRUE(reply);
  const auto& error = std::get<JobFailed>(*reply);
  EXPECT_EQ(error.id, 5u);
  EXPECT_EQ(error.error.code, ErrorCode::kAmbiguous);
}

}  // namespace
}  // namespace manbo
