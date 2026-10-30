#include "repository_setup.h"

#include <mutex>

#include "repositories/manual_repository.h"
#include "repositories/offline_repository.h"
#include "repositories/sample_repository.h"

namespace manbo {
namespace {
// Catalog discovery/loading is backend work: the first request reaches this
// object only after Session has rendered and flushed its acknowledgement.
class LazyOfflineRepository final : public IDocumentRepository {
 public:
  explicit LazyOfflineRepository(RepositoryOptions options)
      : options_(std::move(options)) {
  }

  auto
  Search(std::string_view query, std::stop_token stop)
    -> Result<std::vector<SearchHit>> override {
    auto ready = Ensure(stop);
    if (!ready)
      return tl::make_unexpected(ready.error());
    return repository_
             ? repository_->Search(query, stop)
             : Result<std::vector<SearchHit>>{std::vector<SearchHit>{}};
  }

  auto
  Read(std::string_view reference, std::stop_token stop)
    -> Result<Document> override {
    auto ready = Ensure(stop);
    if (!ready)
      return tl::make_unexpected(ready.error());
    if (repository_)
      return repository_->Read(reference, stop);
    return tl::make_unexpected(Error{ErrorCode::kNotFound,
      "No offline catalog installed; use manbo manbo for setup"});
  }

  Result<LinkedDocument>
  Follow(std::string_view page_id,
    std::string_view target,
    std::stop_token stop) override {
    auto ready = Ensure(stop);
    if (!ready)
      return tl::make_unexpected(ready.error());
    if (repository_)
      return repository_->Follow(page_id, target, stop);
    return IDocumentRepository::Follow(page_id, target, stop);
  }

 private:
  auto
  Ensure(std::stop_token stop) -> Result<void> {
    if (stop.stop_requested())
      return tl::make_unexpected(
        Error{ErrorCode::kCancelled, "Offline request cancelled"});
    std::call_once(opened_, [&] {
      const auto directory = OfflineDirectory(options_);
      if (directory.empty())
        return;
      auto repository = OfflineRepository::Open(directory);
      if (repository)
        repository_ = std::move(*repository);
      else
        error_ = repository.error();
    });
    if (stop.stop_requested())
      return tl::make_unexpected(
        Error{ErrorCode::kCancelled, "Offline request cancelled"});
    if (error_)
      return tl::make_unexpected(*error_);
    return {};
  }
  RepositoryOptions options_;
  std::once_flag opened_;
  std::unique_ptr<OfflineRepository> repository_;
  std::optional<Error> error_;
};
}  // namespace

auto
OfflineDirectory(const RepositoryOptions& options) -> std::filesystem::path {
  if (!options.directory.empty())
    return options.directory;
  if (options.samples)
    return {};
  const auto executable = std::filesystem::absolute(
    options.executable.empty() ? std::filesystem::path("manbo")
                               : options.executable);
  const auto find_catalog = [](std::filesystem::path search_root) {
    while (!search_root.empty()) {
      for (const auto& relative : {"data/library", "data/official-samples"}) {
        const auto candidate = search_root / relative;
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate / "catalog.json", error))
          return candidate;
      }

      const auto parent = search_root.parent_path();
      if (parent == search_root)
        break;
      search_root = parent;
    }
    return std::filesystem::path{};
  };
  for (const auto& search_root :
    {std::filesystem::current_path(), executable.parent_path()}) {
    auto catalog_directory = find_catalog(search_root);
    if (!catalog_directory.empty())
      return catalog_directory;
  }
  return {};
}

auto
OpenAppRepository(const RepositoryOptions& options)
  -> Result<std::unique_ptr<IDocumentRepository>> {
  if (options.samples && !options.directory.empty())
    return tl::make_unexpected(
      Error{ErrorCode::kInvalidArgument, "Choose either --samples or --data"});
  if (options.samples)
    return WithManboManual(std::make_unique<SampleRepository>());
  return WithManboManual(std::make_unique<LazyOfflineRepository>(options));
}

auto
ProtectSourceOutput(const RepositoryOptions& options,
  const std::filesystem::path& output) -> Result<void> {
  const auto directory = OfflineDirectory(options);
  if (directory.empty())
    return {};
  // Compare canonical existing ancestors, not lexical path prefixes.
  auto parent = std::filesystem::weakly_canonical(output).parent_path();
  while (!parent.empty()) {
    std::error_code error;
    if (std::filesystem::equivalent(parent, directory, error))
      return tl::make_unexpected(Error{ErrorCode::kInvalidArgument,
        "Frame output must not overwrite the offline source catalog or its "
        "files"});
    const auto next = parent.parent_path();
    if (next == parent)
      break;
    parent = next;
  }
  return {};
}
}  // namespace manbo
