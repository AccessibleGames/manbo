#ifndef MANBO_TERMINAL_TERMINAL_H_
#define MANBO_TERMINAL_TERMINAL_H_

#include <memory>

#include "core/interfaces.h"
#include "runtime/channel.h"

namespace manbo {

class IConsole;

struct TerminalMetrics {
  std::uint64_t frame_bytes = 0;
  std::uint64_t native_waits = 0;
  std::uint64_t input_records = 0;
};

class ITerminal : public IEventSource, public IRenderer {
 public:
  virtual TerminalMetrics
  metrics() const noexcept = 0;
};

bool
TerminalAvailable() noexcept;
Result<std::unique_ptr<ITerminal>>
MakeTerminal(EventChannel& replies);
// Inject a console transport for frontend tests or a different terminal host.
Result<std::unique_ptr<ITerminal>>
MakeTerminal(EventChannel& replies, std::unique_ptr<IConsole> console);

}  // namespace manbo

#endif  // MANBO_TERMINAL_TERMINAL_H_
