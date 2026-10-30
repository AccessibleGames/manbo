#ifndef MANBO_CHANNEL_H_
#define MANBO_CHANNEL_H_

#include <cstddef>
#include <memory>
#include <optional>
#include <stop_token>

#include "core/types.h"

namespace manbo {

class WakeSignal;

// Boost.Asio channel adapter for the synchronous native-console boundary.
class EventChannel {
 public:
  static Result<std::unique_ptr<EventChannel>>
  Create(std::size_t capacity = 64);
  ~EventChannel();
  EventChannel(const EventChannel&) = delete;
  EventChannel&
  operator=(const EventChannel&) = delete;

  bool
  Send(Event event, std::stop_token stop = {});
  bool
  TrySend(Event event);
  std::optional<Event>
  Receive(std::stop_token stop = {});
  std::optional<Event>
  TryReceive();
  void
  Close() noexcept;
  [[nodiscard]] bool
  closed() const;
  [[nodiscard]] WakeSignal&
  wake_signal() noexcept;

 private:
  struct Impl;
  explicit EventChannel(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace manbo

#endif  // MANBO_CHANNEL_H_
