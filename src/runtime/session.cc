#include "runtime/session.h"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <exception>
#include <utility>

namespace manbo {
namespace asio = boost::asio;

Result<void>
Session::Apply(Transition transition) {
  if (transition.visible_change) {
    auto rendered = renderer_.Render(browser_.state());
    if (!rendered)
      return tl::make_unexpected(rendered.error());
    ++metrics_.renders;
    auto flushed = renderer_.Flush();
    if (!flushed)
      return tl::make_unexpected(flushed.error());
    ++metrics_.flushes;
  }
  // Keep terminal output visible before costly backend work starts.
  if (transition.cancel_work)
    jobs_.Cancel();

  if (transition.request) {
    const auto request_id = GetRequestId(*transition.request);
    ++metrics_.requests;
    auto submitted = jobs_.Submit(std::move(*transition.request));
    if (!submitted)
      return Apply(browser_.Dispatch(JobFailed{request_id, submitted.error()}));
  }
  return {};
}

asio::awaitable<Result<void>>
Session::Consume(EventStream& stream, Event initial) {
  auto initial_result = Apply(browser_.Dispatch(initial));
  if (!initial_result)
    co_return tl::make_unexpected(initial_result.error());

  while (browser_.state().running) {
    ++metrics_.waits;

    auto [error, incoming_event] =
      co_await stream.async_receive(asio::as_tuple(asio::use_awaitable));
    if (error)
      co_return tl::make_unexpected(Error{ErrorCode::kClosed, error.message()});
    ++metrics_.events;
    auto transition_result = Apply(browser_.Dispatch(incoming_event));
    if (!transition_result)
      co_return tl::make_unexpected(transition_result.error());
  }
  co_return Result<void>{};
}

Result<SessionMetrics>
Session::Run(std::string initial_query) {
  return RunInitial(Search{std::move(initial_query)});
}

Result<SessionMetrics>
Session::RunInitial(Event initial) {
  if (!std::holds_alternative<Search>(initial) &&
      !std::holds_alternative<ShowSearch>(initial) &&
      !std::holds_alternative<OpenPage>(initial))
    return tl::make_unexpected(Error{ErrorCode::kInvalidArgument,
      "A session starts with a search prompt, query, or page reference"});
  if (browser_.state().generation != 0)
    return tl::make_unexpected(
      Error{ErrorCode::kInvalidArgument, "A Session can only be run once"});
  try {
    asio::io_context ui_context;
    EventStream stream(ui_context, 1);
    std::optional<Result<void>> consumer_result;
    asio::co_spawn(ui_context,
      Consume(stream, std::move(initial)),
      [&](std::exception_ptr exception, Result<void> result) {
        if (exception)
          std::rethrow_exception(exception);
        consumer_result = std::move(result);
      });
    ui_context.poll();
    while (!consumer_result) {
      // Native input blocks here. Dispatch Asio work only after actual events;
      // no recurring poll, extra UI thread, or timer is needed.
      auto next_event_result = events_.Next();
      if (!next_event_result) {
        jobs_.Shutdown();
        return tl::make_unexpected(next_event_result.error());
      }
      Event incoming_event =
        *next_event_result ? std::move(**next_event_result) : Event{Quit{}};
      if (!stream.try_send(
            boost::system::error_code{}, std::move(incoming_event))) {
        jobs_.Shutdown();
        return tl::make_unexpected(
          Error{ErrorCode::kBackend, "Event stream is full"});
      }
      ui_context.poll();
    }
    jobs_.Shutdown();
    if (!*consumer_result)
      return tl::make_unexpected(consumer_result->error());
    return metrics_;
  } catch (const std::exception& error) {
    jobs_.Shutdown();
    return tl::make_unexpected(Error{ErrorCode::kBackend, error.what()});
  }
}

}  // namespace manbo
