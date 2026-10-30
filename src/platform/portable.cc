// Headless-only fallback: no POSIX headers, native handles, or platform
// polling.
#include <utility>

#include "platform/console.h"
#include "platform/wake_signal.h"

namespace manbo {

struct WakeSignal::Impl {};
WakeSignal::WakeSignal(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {
}
WakeSignal::~WakeSignal() = default;
Result<std::unique_ptr<WakeSignal>>
WakeSignal::Create() {
  return std::unique_ptr<WakeSignal>(new WakeSignal(std::make_unique<Impl>()));
}
void
WakeSignal::Notify() noexcept {
}
void
WakeSignal::Reset() noexcept {
}

bool
ConsoleAvailable() noexcept {
  return false;
}

Result<std::unique_ptr<IConsole>>
OpenConsole() {
  return tl::make_unexpected(Error{
    ErrorCode::kPlatform, "This build has no interactive console backend"});
}

}  // namespace manbo
