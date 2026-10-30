#include <fmt/format.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <semaphore>
#include <thread>

#include "runtime/jobs.h"
#include "runtime/session.h"
#include "support/fixtures.h"

namespace manbo {
namespace {
using namespace std::chrono_literals;

using test::ExampleDocument;
using test::ExampleHit;

class IdleRepository final : public IDocumentRepository {
 public:
  Result<std::vector<SearchHit>>
  Search(std::string_view, std::stop_token) override {
    ++calls;
    return std::vector<SearchHit>{ExampleHit()};
  }

  Result<Document>
  Read(std::string_view, std::stop_token) override {
    ++reads;
    return ExampleDocument();
  }
  std::atomic<int> calls = 0;
  std::atomic<int> reads = 0;
};

class IdleEvents final : public IEventSource {
 public:
  explicit IdleEvents(EventChannel& channel) : channel_(channel) {
  }

  Result<std::optional<Event>>
  Next() override {
    if (++waits == 3)
      settled.release();
    return channel_.Receive();
  }
  std::atomic<int> waits = 0;
  std::binary_semaphore settled{0};

 private:
  EventChannel& channel_;
};

class IdleRenderer final : public IRenderer {
 public:
  Result<void>
  Render(const Snapshot& snapshot) override {
    frame_bytes += SerializeAnsi(RenderFrame(snapshot, 100, 25)).size();
    ++renders;
    return {};
  }

  Result<void>
  Flush() override {
    ++flushes;
    return {};
  }
  std::atomic<int> renders = 0;
  std::atomic<int> flushes = 0;
  std::atomic<std::uint64_t> frame_bytes = 0;
};

TEST(Idle, Quiescent) {
  auto channel = EventChannel::Create();
  ASSERT_TRUE(channel);
  IdleRepository repository;
  auto jobs = MakeJobService(repository, **channel, 1);
  ASSERT_TRUE(jobs);
  IdleEvents events(**channel);
  IdleRenderer renderer;
  Result<SessionMetrics> result =
    tl::make_unexpected(Error{ErrorCode::kBackend, "Not run"});
  std::jthread owner([&] {
    Session session(events, renderer, **jobs);
    result = session.Run();
  });
  events.settled.acquire();  // After the actual preview frame, at the third
                             // blocking receive.
  const int renders = renderer.renders.load();
  const int flushes = renderer.flushes.load();
  const auto bytes = renderer.frame_bytes.load();
  const char* configured = std::getenv("MANBO_IDLE_SECONDS");
  const auto duration = configured
                          ? std::chrono::milliseconds(
                              std::clamp(std::atoi(configured), 1, 120) * 1000)
                          : 200ms;
  // Test-only one-shot observation deadline. The application arms no timer.
  std::this_thread::sleep_for(duration);
  EXPECT_EQ(renderer.renders.load(), renders);
  EXPECT_EQ(renderer.flushes.load(), flushes);
  EXPECT_EQ(renderer.frame_bytes.load(), bytes);
  EXPECT_EQ(repository.calls.load(), 1);
  EXPECT_EQ(repository.reads.load(), 1);
  EXPECT_EQ(events.waits.load(), 3);
  EXPECT_TRUE((*channel)->Send(Quit{}));
  owner.join();
  ASSERT_TRUE(result);
  EXPECT_EQ(result->requests, 2u);
  std::cout << fmt::format(
    "IDLE_EVIDENCE duration_ms={} renders={} flushes={} "
    "frame_bytes={} requests={} blocking_receives={}\n",
    duration.count(),
    renders,
    flushes,
    bytes,
    result->requests,
    events.waits.load());
}

}  // namespace
}  // namespace manbo
