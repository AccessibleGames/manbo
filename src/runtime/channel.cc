#include "runtime/channel.h"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/consign.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_future.hpp>
#include <future>
#include <mutex>
#include <tuple>
#include <utility>

#include "platform/wake_signal.h"

namespace manbo {
namespace asio = boost::asio;
namespace {
// Adapt Asio's future token to the synchronous console boundary. Consign keeps
// the cancellation signal alive through completion, even if the caller stops.
auto
CompletionToken(asio::thread_pool& executor,
  const std::shared_ptr<asio::cancellation_signal>& cancellation) {
  return asio::bind_executor(executor.get_executor(),
    asio::bind_cancellation_slot(cancellation->slot(),
      asio::consign(asio::as_tuple(asio::use_future), cancellation)));
}
}  // namespace

struct EventChannel::Impl {
  Impl(std::size_t capacity, std::unique_ptr<WakeSignal> signal)
      : messages(completions, capacity), wake(std::move(signal)) {
  }

  void
  RefreshWake() noexcept {
    // Admission and wake updates share this lock so a reset cannot hide a send.
    wake->Reset();
    if (messages.ready() || closed)
      wake->Notify();
  }

  // Blocked repository workers must not occupy the executor that releases
  // their sends or cancellations. Completion handlers never render the UI.
  asio::thread_pool completions{1};
  asio::experimental::concurrent_channel<asio::thread_pool::executor_type,
    void(boost::system::error_code, Event)>
    messages;
  std::unique_ptr<WakeSignal> wake;
  // Serializes native wake updates with admission and closure.
  mutable std::mutex admission;
  bool closed = false;
};

Result<std::unique_ptr<EventChannel>>
EventChannel::Create(std::size_t capacity) {
  if (capacity == 0)
    return tl::make_unexpected(
      Error{ErrorCode::kInvalidArgument, "Channel capacity must be positive"});
  auto wake = WakeSignal::Create();
  if (!wake)
    return tl::make_unexpected(wake.error());
  return std::unique_ptr<EventChannel>(
    new EventChannel(std::make_unique<Impl>(capacity, std::move(*wake))));
}

EventChannel::EventChannel(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {
}

EventChannel::~EventChannel() {
  Close();
  impl_->completions.join();
}

bool
EventChannel::TrySend(Event event) {
  std::lock_guard lock(impl_->admission);
  if (impl_->closed ||
      !impl_->messages.try_send(boost::system::error_code{}, std::move(event)))
    return false;
  impl_->wake->Notify();
  return true;
}

bool
EventChannel::Send(Event event, std::stop_token stop) {
  if (stop.stop_requested())
    return false;
  auto cancellation = std::make_shared<asio::cancellation_signal>();
  std::future<std::tuple<boost::system::error_code>> completion;
  {
    std::lock_guard lock(impl_->admission);
    if (impl_->closed)
      return false;
    completion = impl_->messages.async_send(boost::system::error_code{},
      std::move(event),
      CompletionToken(impl_->completions, cancellation));
    impl_->wake->Notify();
  }
  std::stop_callback cancel_on_stop(stop, [this, cancellation] {
    asio::post(impl_->completions,
      [cancellation] { cancellation->emit(asio::cancellation_type::all); });
  });
  const auto [error] = completion.get();
  return !error;
}

std::optional<Event>
EventChannel::TryReceive() {
  std::lock_guard lock(impl_->admission);
  std::optional<Event> event;
  impl_->messages.try_receive(
    [&](boost::system::error_code error, Event incoming) {
      if (!error)
        event = std::move(incoming);
    });
  impl_->RefreshWake();
  return event;
}

std::optional<Event>
EventChannel::Receive(std::stop_token stop) {
  if (stop.stop_requested())
    return std::nullopt;
  if (auto event = TryReceive())
    return event;
  auto cancellation = std::make_shared<asio::cancellation_signal>();
  std::future<std::tuple<boost::system::error_code, Event>> completion;
  {
    std::lock_guard lock(impl_->admission);
    completion = impl_->messages.async_receive(
      CompletionToken(impl_->completions, cancellation));
    impl_->RefreshWake();
  }
  std::stop_callback cancel_on_stop(stop, [this, cancellation] {
    // Cancellation and completions share the same Asio executor.
    asio::post(impl_->completions,
      [cancellation] { cancellation->emit(asio::cancellation_type::all); });
  });
  auto [error, event] = completion.get();
  if (error)
    return std::nullopt;
  return event;
}

void
EventChannel::Close() noexcept {
  std::lock_guard lock(impl_->admission);
  if (impl_->closed)
    return;
  impl_->closed = true;
  // Cancel blocked senders before closing; buffered messages remain readable.
  impl_->messages.cancel();
  impl_->messages.close();
  impl_->wake->Notify();
}

bool
EventChannel::closed() const {
  std::lock_guard lock(impl_->admission);
  return impl_->closed;
}

WakeSignal&
EventChannel::wake_signal() noexcept {
  return *impl_->wake;
}

}  // namespace manbo
