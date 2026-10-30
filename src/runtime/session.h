#ifndef MANBO_SESSION_H_
#define MANBO_SESSION_H_

#include <boost/asio/awaitable.hpp>
#include <boost/asio/experimental/channel.hpp>

#include "core/browser.h"
#include "core/interfaces.h"

namespace manbo {

// Composition only: Browser itself has no frontend, backend, or scheduler.
class Session {
 public:
  Session(IEventSource& events, IRenderer& renderer, IJobService& jobs)
      : events_(events), renderer_(renderer), jobs_(jobs) {
  }

  Result<SessionMetrics>
  Run(std::string initial_query = {});
  Result<SessionMetrics>
  RunInitial(Event initial);
  [[nodiscard]]
  const Browser&
  browser() const noexcept {
    return browser_;
  }

 private:
  Result<void>
  Apply(Transition transition);
  using EventStream =
    boost::asio::experimental::channel<void(boost::system::error_code, Event)>;
  boost::asio::awaitable<Result<void>>
  Consume(EventStream& stream, Event initial);
  IEventSource& events_;
  IRenderer& renderer_;
  IJobService& jobs_;
  Browser browser_;
  SessionMetrics metrics_;
};

}  // namespace manbo

#endif  // MANBO_SESSION_H_
