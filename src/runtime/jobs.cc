#include "runtime/jobs.h"

#include <boost/asio/post.hpp>
#include <boost/asio/thread_pool.hpp>
#include <exception>
#include <mutex>
#include <type_traits>
#include <utility>

namespace manbo {
namespace {

std::optional<Event>
ExecuteRequest(IDocumentRepository& document_repository,
  const Request& job_request,
  std::stop_token stop_token) {
  if (stop_token.stop_requested())
    return std::nullopt;
  return std::visit(
    [&](const auto& request_payload) -> std::optional<Event> {
      using T = std::decay_t<decltype(request_payload)>;
      if constexpr (std::is_same_v<T, SearchRequest>) {
        auto search_hits =
          document_repository.Search(request_payload.query, stop_token);
        if (stop_token.stop_requested())
          return std::nullopt;
        if (!search_hits)
          return JobFailed{request_payload.id, search_hits.error()};
        return SearchCompleted{request_payload.id, std::move(*search_hits)};
      } else if constexpr (std::is_same_v<T, ReadRequest>) {
        auto document =
          document_repository.Read(request_payload.page_id, stop_token);
        if (stop_token.stop_requested())
          return std::nullopt;
        if (!document)
          return JobFailed{request_payload.id, document.error()};
        return ReadCompleted{
          request_payload.id, std::move(*document), request_payload.preview};
      } else {
        auto page = document_repository.Follow(
          request_payload.page_id, request_payload.target, stop_token);
        if (stop_token.stop_requested())
          return std::nullopt;
        if (!page)
          return JobFailed{request_payload.id, page.error()};
        return LinkCompleted{request_payload.id, std::move(*page)};
      }
    },
    job_request);
}

// Contain exceptions from third-party/std resource operations at this boundary.
std::optional<Event>
ExecuteSafely(IDocumentRepository& document_repository,
  const Request& job_request,
  std::stop_token stop_token) {
  try {
    return ExecuteRequest(document_repository, job_request, stop_token);
  } catch (const std::exception& error) {
    if (stop_token.stop_requested())
      return std::nullopt;
    return JobFailed{
      GetRequestId(job_request), {ErrorCode::kBackend, error.what()}};
  } catch (...) {
    if (stop_token.stop_requested())
      return std::nullopt;
    return JobFailed{GetRequestId(job_request),
      {ErrorCode::kBackend, "Unexpected repository failure"}};
  }
}

class InlineJobs final : public IJobService {
 public:
  InlineJobs(
    IDocumentRepository& document_repository, EventChannel& reply_channel)
      : document_repository_(document_repository),
        reply_channel_(reply_channel) {
  }

  Result<void>
  Submit(Request job_request) override {
    if (closed_)
      return tl::make_unexpected(
        Error{ErrorCode::kClosed, "Job service is closed"});
    if (auto completion_event =
          ExecuteSafely(document_repository_, job_request, {})) {
      // A sender must never block a mailbox consumed by the same thread.
      if (!reply_channel_.TrySend(std::move(*completion_event)))
        return tl::make_unexpected(
          Error{ErrorCode::kClosed, "Inline reply mailbox is full or closed"});
    }
    return {};
  }

  void
  Cancel() noexcept override {
  }

  void
  Shutdown() noexcept override {
    closed_ = true;
  }

 private:
  IDocumentRepository& document_repository_;
  EventChannel& reply_channel_;
  bool closed_ = false;
};

class WorkerJobs final : public IJobService {
 public:
  WorkerJobs(IDocumentRepository& document_repository,
    EventChannel& reply_channel,
    std::size_t worker_count)
      : document_repository_(document_repository),
        reply_channel_(reply_channel),
        worker_count_(worker_count),
        worker_pool_(worker_count) {
  }

  ~WorkerJobs() override {
    Shutdown();
  }

  Result<void>
  Submit(Request job_request) override {
    std::stop_source previous_stop_source;
    {
      std::lock_guard lock(mutex_);
      if (closed_)
        return tl::make_unexpected(
          Error{ErrorCode::kClosed, "Job service is closed"});
      previous_stop_source = active_stop_source_;
      active_stop_source_ = std::stop_source{};
      pending_work_ =
        Work{std::move(job_request), active_stop_source_.get_token()};
      if (scheduled_workers_ < worker_count_) {
        boost::asio::post(worker_pool_, [this] { RunPendingRequests(); });
        ++scheduled_workers_;
      }
    }
    // Stop callbacks can re-enter services, so invoke them outside this lock.
    previous_stop_source.request_stop();
    return {};
  }

  void
  Cancel() noexcept override {
    std::stop_source previous_stop_source;
    {
      std::lock_guard lock(mutex_);
      previous_stop_source = active_stop_source_;
      pending_work_.reset();
    }
    previous_stop_source.request_stop();
  }

  void
  Shutdown() noexcept override {
    {
      std::lock_guard lock(mutex_);
      if (closed_)
        return;
      closed_ = true;
    }
    Cancel();
    worker_pool_.stop();
    worker_pool_.join();
  }

 private:
  struct Work {
    Request job_request;
    std::stop_token cancellation_token;
  };

  void
  RunPendingRequests() {
    for (;;) {
      std::optional<Work> selected_work;
      {
        std::lock_guard lock(mutex_);
        if (closed_ || !pending_work_) {
          --scheduled_workers_;
          return;
        }
        selected_work = std::move(pending_work_);
        pending_work_.reset();
      }
      if (auto reply = ExecuteSafely(document_repository_,
            selected_work->job_request,
            selected_work->cancellation_token))
        reply_channel_.Send(
          std::move(*reply), selected_work->cancellation_token);
    }
  }

  IDocumentRepository& document_repository_;
  EventChannel& reply_channel_;
  const std::size_t worker_count_;
  std::mutex mutex_;
  std::optional<Work> pending_work_;
  std::stop_source active_stop_source_;
  std::size_t scheduled_workers_ = 0;
  bool closed_ = false;
  // Destroy/join the Boost pool before any state its tasks reference.
  boost::asio::thread_pool worker_pool_;
};
}  // namespace

Result<std::unique_ptr<IJobService>>
MakeJobService(IDocumentRepository& document_repository,
  EventChannel& reply_channel,
  std::size_t worker_count) {
  if (worker_count > 16)
    return tl::make_unexpected(Error{
      ErrorCode::kInvalidArgument, "Worker count must be between 0 and 16"});
  try {
    if (worker_count == 0)
      return std::unique_ptr<IJobService>(
        new InlineJobs(document_repository, reply_channel));
    return std::unique_ptr<IJobService>(
      new WorkerJobs(document_repository, reply_channel, worker_count));
  } catch (const std::exception& error) {
    return tl::make_unexpected(Error{ErrorCode::kBackend, error.what()});
  }
}

}  // namespace manbo
