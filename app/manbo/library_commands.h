#ifndef MANBO_APP_LIBRARY_COMMANDS_H_
#define MANBO_APP_LIBRARY_COMMANDS_H_

#include <optional>

#include "library/library.h"

namespace CLI {
class App;
}

namespace manbo {
class LibraryCommands {
 public:
  LibraryCommands(CLI::App& app, std::string& directory);

  std::optional<Result<std::string>>
  Run() const;

  bool
  selected() const;

 private:
  CLI::App* import_;
  CLI::App* html_;
  CLI::App* text_;
  CLI::App* index_;
  CLI::App* format_;
  std::string& directory_;
  ImportOptions options_;
  std::string family_ = "generic";
  std::filesystem::path format_source_;
  std::filesystem::path format_output_;
  int format_width_ = 100;
};
}  // namespace manbo

#endif  // MANBO_APP_LIBRARY_COMMANDS_H_
