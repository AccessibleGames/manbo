#ifndef MANBO_PLATFORM_CONSOLE_H_
#define MANBO_PLATFORM_CONSOLE_H_

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "core/result.h"
#include "platform/wake_signal.h"

namespace manbo {

// Normalized console transport values. No browser routes, view objects, native
// handles, virtual-key constants, or terminal command bindings cross this API.
enum class ConsoleEventKind { kWake, kResize, kKey, kWheel };
enum class ConsoleKey {
  kCharacter,
  kEscape,
  kEnter,
  kUp,
  kDown,
  kPageUp,
  kPageDown,
  kBackspace,
  kTab,
  kBackTab
};
struct ConsoleEvent {
  ConsoleEventKind kind = ConsoleEventKind::kWake;
  ConsoleKey key = ConsoleKey::kCharacter;
  std::string text = {};
  char shortcut = 0;  // Uppercase ASCII letter for Ctrl/Alt combinations.
  int repeat = 1;
  bool control = false;
  bool alt = false;
  int wheel_steps = 0;
};
struct ConsoleSize {
  int width;
  int height;
};
struct ConsoleMetrics {
  std::uint64_t waits = 0;
  std::uint64_t input_records = 0;
};

class IConsole {
 public:
  virtual ~IConsole() = default;
  // Block on real console input or the supplied inbox wake signal. No timer.
  virtual Result<ConsoleEvent>
  Next(WakeSignal& wake, bool prefer_wake) = 0;
  virtual Result<ConsoleSize>
  Size() const = 0;
  virtual Result<void>
  Write(std::string_view bytes) = 0;
  virtual ConsoleMetrics
  metrics() const noexcept = 0;
};

bool
ConsoleAvailable() noexcept;
// Owns console modes/alternate-screen restoration. Fallible setup is explicit.
Result<std::unique_ptr<IConsole>>
OpenConsole();

}  // namespace manbo

#endif  // MANBO_PLATFORM_CONSOLE_H_
