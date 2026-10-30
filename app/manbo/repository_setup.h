#ifndef MANBO_APP_MANBO_REPOSITORY_SETUP_H_
#define MANBO_APP_MANBO_REPOSITORY_SETUP_H_

#include <filesystem>
#include <memory>
#include <string>

#include "core/interfaces.h"

namespace manbo {
struct RepositoryOptions {
  std::string directory;
  bool samples = false;
  std::filesystem::path executable;
};
// Application startup policy, not a library/global repository singleton.
auto
OfflineDirectory(const RepositoryOptions& options) -> std::filesystem::path;
auto
OpenAppRepository(const RepositoryOptions& options)
  -> Result<std::unique_ptr<IDocumentRepository>>;
auto
ProtectSourceOutput(const RepositoryOptions& options,
  const std::filesystem::path& output) -> Result<void>;
}  // namespace manbo

#endif  // MANBO_APP_MANBO_REPOSITORY_SETUP_H_
