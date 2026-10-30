#include "runtime/channel.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <future>
#include <ranges>
#include <semaphore>
#include <thread>
#include <vector>

namespace manbo {
namespace {
using namespace std::chrono_literals;

TEST(Channel, CapacityAndClosureHaveExplicitSemantics) {
  auto invalid = EventChannel::Create(0);
  ASSERT_FALSE(invalid);
  EXPECT_EQ(invalid.error().code, ErrorCode::kInvalidArgument);
  auto channel = EventChannel::Create(1);
  ASSERT_TRUE(channel);
  EXPECT_TRUE((*channel)->TrySend(Search{"one"}));
  EXPECT_FALSE((*channel)->TrySend(Search{"two"}));
  (*channel)->Close();
  EXPECT_TRUE((*channel)->closed());
  EXPECT_FALSE((*channel)->TrySend(Quit{}));
  ASSERT_TRUE((*channel)->Receive());
  EXPECT_TRUE((*channel)->closed());
  EXPECT_FALSE((*channel)->Receive());
}

TEST(Channel, EmptyReceiverWakesForARealMessage) {
  auto channel = EventChannel::Create();
  ASSERT_TRUE(channel);
  std::promise<std::optional<Event>> promise;
  auto future = promise.get_future();
  std::jthread receiver([&](std::stop_token stop) {
    promise.set_value((*channel)->Receive(stop));
  });
  EXPECT_TRUE((*channel)->Send(Search{"wake"}));
  if (future.wait_for(2s) != std::future_status::ready)
    receiver.request_stop();
  auto event = future.get();
  ASSERT_TRUE(event);
  EXPECT_EQ(std::get<Search>(*event).query, "wake");
}

TEST(Channel, FullProducerResumesWithoutLosingReliableMessages) {
  auto channel = EventChannel::Create(1);
  ASSERT_TRUE(channel);
  ASSERT_TRUE((*channel)->Send(Search{"first"}));
  std::binary_semaphore entered(0);
  std::promise<bool> promise;
  auto future = promise.get_future();
  std::jthread producer([&](std::stop_token stop) {
    entered.release();
    promise.set_value((*channel)->Send(Search{"second"}, stop));
  });
  entered.acquire();
  EXPECT_FALSE((*channel)->TrySend(Search{"overflow"}));
  auto first = (*channel)->TryReceive();
  if (future.wait_for(2s) != std::future_status::ready)
    producer.request_stop();
  EXPECT_TRUE(future.get());
  auto second = (*channel)->TryReceive();
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  EXPECT_EQ(std::get<Search>(*first).query, "first");
  EXPECT_EQ(std::get<Search>(*second).query, "second");
}

TEST(Channel, CancellationReleasesAFullProducer) {
  auto channel = EventChannel::Create(1);
  ASSERT_TRUE(channel);
  ASSERT_TRUE((*channel)->Send(Search{"kept"}));
  std::promise<bool> promise;
  auto future = promise.get_future();
  std::jthread producer([&](std::stop_token stop) {
    promise.set_value((*channel)->Send(Quit{}, stop));
  });
  EXPECT_EQ(future.wait_for(10ms), std::future_status::timeout);
  producer.request_stop();
  EXPECT_FALSE(future.get());
  auto kept = (*channel)->TryReceive();
  ASSERT_TRUE(kept);
  EXPECT_EQ(std::get<Search>(*kept).query, "kept");
}

TEST(Channel, ClosingReleasesAnEmptyReceiver) {
  auto channel = EventChannel::Create();
  ASSERT_TRUE(channel);
  std::promise<std::optional<Event>> promise;
  auto future = promise.get_future();
  std::jthread receiver([&] { promise.set_value((*channel)->Receive()); });
  (*channel)->Close();
  EXPECT_EQ(future.wait_for(2s), std::future_status::ready);
  EXPECT_FALSE(future.get());
}

TEST(Channel, ClosingReleasesAFullProducerAndPreservesBufferedMessages) {
  auto channel = EventChannel::Create(1);
  ASSERT_TRUE(channel);
  ASSERT_TRUE((*channel)->TrySend(Search{"buffered"}));
  std::promise<bool> promise;
  auto future = promise.get_future();
  std::binary_semaphore entered(0);
  std::jthread producer([&](std::stop_token stop) {
    entered.release();
    promise.set_value((*channel)->Send(Search{"blocked"}, stop));
  });
  entered.acquire();
  EXPECT_EQ(future.wait_for(10ms), std::future_status::timeout);
  (*channel)->Close();
  if (future.wait_for(2s) != std::future_status::ready)
    EXPECT_EQ(future.wait_for(10ms), std::future_status::timeout);
  producer.request_stop();
  EXPECT_FALSE(future.get());
  auto buffered = (*channel)->Receive();
  ASSERT_TRUE(buffered);
  EXPECT_EQ(std::get<Search>(*buffered).query, "buffered");
  EXPECT_FALSE((*channel)->Receive());
}

TEST(Channel, CancellingAReceiverLeavesTheChannelUsable) {
  auto channel = EventChannel::Create(1);
  ASSERT_TRUE(channel);
  std::promise<std::optional<Event>> promise;
  auto future = promise.get_future();
  std::jthread receiver([&](std::stop_token stop) {
    promise.set_value((*channel)->Receive(stop));
  });
  EXPECT_EQ(future.wait_for(10ms), std::future_status::timeout);
  receiver.request_stop();
  ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
  EXPECT_FALSE(future.get());
  ASSERT_TRUE((*channel)->TrySend(Search{"next"}));
  auto next = (*channel)->Receive();
  ASSERT_TRUE(next);
  EXPECT_EQ(std::get<Search>(*next).query, "next");
}

TEST(Channel, ConcurrentReliableDeliveryHasNoLostWakeups) {
  auto channel = EventChannel::Create(3);
  ASSERT_TRUE(channel);
  constexpr int kProducers = 3;
  constexpr int kPerProducer = 300;
  std::vector<std::jthread> producers;
  for (int lane = 0; lane < kProducers; ++lane)
    producers.emplace_back([&, lane](std::stop_token stop) {
      for (int n = 0; n < kPerProducer; ++n)
        if (!(*channel)->Send(
              SearchCompleted{
                static_cast<RequestId>(lane * kPerProducer + n), {}},
              stop))
          return;
    });
  std::vector<bool> seen(kProducers * kPerProducer);
  for (int n = 0; n < kProducers * kPerProducer; ++n) {
    auto event = (*channel)->Receive();
    ASSERT_TRUE(event);
    const auto id = std::get<SearchCompleted>(*event).id;
    ASSERT_LT(id, seen.size());
    EXPECT_FALSE(seen[static_cast<std::size_t>(id)]);
    seen[static_cast<std::size_t>(id)] = true;
  }

  EXPECT_TRUE(std::ranges::all_of(seen, [](bool value) { return value; }));
}

}  // namespace
}  // namespace manbo
