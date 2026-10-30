#ifndef MANBO_WAKE_SIGNAL_H_
#define MANBO_WAKE_SIGNAL_H_

#include <memory>

#include "core/result.h"

namespace manbo {

class NativeConsole;

// Opaque wake primitive. Native types stay in the single private OS backend.
class WakeSignal {
 public:
  static Result<std::unique_ptr<WakeSignal>>
  Create();
  ~WakeSignal();
  WakeSignal(const WakeSignal&) = delete;
  WakeSignal&
  operator=(const WakeSignal&) = delete;
  void
  Notify() noexcept;
  void
  Reset() noexcept;

 private:
  struct Impl;
  explicit WakeSignal(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
  friend class NativeConsole;
};

}  // namespace manbo

#endif  // MANBO_WAKE_SIGNAL_H_
