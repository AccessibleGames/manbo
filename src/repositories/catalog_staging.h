#ifndef MANBO_REPOSITORIES_CATALOG_STAGING_H_
#define MANBO_REPOSITORIES_CATALOG_STAGING_H_

#include <filesystem>
#include <random>
#include <stdexcept>

namespace manbo::catalog_internal {
// A sibling staging directory keeps publication on the same filesystem.
// create_directory reserves the name; destruction removes abandoned work.
class StagingDirectory {
 public:
  explicit StagingDirectory(const std::filesystem::path& root) {
    std::random_device random;
    for (int attempt = 0; attempt < 16; ++attempt) {
      path_ = root / (".manbo-pending-" + std::to_string(random()) + "-" +
                       std::to_string(random()));
      if (std::filesystem::create_directory(path_))
        return;
    }
    throw std::runtime_error("Could not reserve an import staging directory");
  }

  ~StagingDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  StagingDirectory(const StagingDirectory&) = delete;
  StagingDirectory&
  operator=(const StagingDirectory&) = delete;

  const std::filesystem::path&
  path() const {
    return path_;
  }

 private:
  std::filesystem::path path_;
};
}  // namespace manbo::catalog_internal

#endif  // MANBO_REPOSITORIES_CATALOG_STAGING_H_
