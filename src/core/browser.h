#ifndef MANBO_BROWSER_H_
#define MANBO_BROWSER_H_

#include "core/types.h"

namespace manbo {

// Application state machine: operations work without a GUI, TUI, or event loop.
class Browser {
 public:
  [[nodiscard]]
  const Snapshot&
  state() const noexcept {
    return state_;
  }

  Transition
  Dispatch(const Event& event);

 private:
  struct ReaderLocation {
    Document document;
    std::size_t scroll_line;
    std::string anchor;
  };
  void
  RememberPage();

  Snapshot state_;
  std::vector<ReaderLocation> history_;
  std::string expected_page_;
  bool direct_open_ = false;
  bool following_link_ = false;
};

}  // namespace manbo

#endif  // MANBO_BROWSER_H_
