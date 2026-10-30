// POSIX terminal transport. UI bindings and rendering remain platform neutral.
#include <fcntl.h>
#include <fmt/format.h>
#include <poll.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <utility>

#include "platform/console.h"

namespace manbo {
namespace {
volatile sig_atomic_t resize_fd = -1;
void
OnResize(int) {
  if (resize_fd >= 0) {
    const char byte = 1;
    (void)::write(resize_fd, &byte, 1);
  }
}

Error
PlatformError(std::string_view operation) {
  return {ErrorCode::kPlatform,
    fmt::format("{} failed: {}", operation, std::strerror(errno))};
}
}  // namespace

struct WakeSignal::Impl {
  int fd = -1;
  ~Impl() {
    if (fd >= 0)
      ::close(fd);
  }
};
WakeSignal::WakeSignal(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {
}
WakeSignal::~WakeSignal() = default;
Result<std::unique_ptr<WakeSignal>>
WakeSignal::Create() {
  auto impl = std::make_unique<Impl>();
  impl->fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (impl->fd < 0)
    return tl::make_unexpected(PlatformError("eventfd"));
  return std::unique_ptr<WakeSignal>(new WakeSignal(std::move(impl)));
}
void
WakeSignal::Notify() noexcept {
  const std::uint64_t one = 1;
  (void)::write(impl_->fd, &one, sizeof(one));
}
void
WakeSignal::Reset() noexcept {
  std::uint64_t count;
  while (::read(impl_->fd, &count, sizeof(count)) > 0) {
  }
}

class NativeConsole final : public IConsole {
 public:
  ~NativeConsole() override {
    if (active_) {
      constexpr std::string_view restore = "\x1b[0m\x1b[?25h\x1b[?1049l";
      (void)::write(STDOUT_FILENO, restore.data(), restore.size());
    }
    if (termios_changed_)
      (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_);
    if (resize_fd == resize_pipe_[1])
      resize_fd = -1;
    if (signal_changed_)
      (void)sigaction(SIGWINCH, &old_action_, nullptr);
    for (int fd : resize_pipe_)
      if (fd >= 0)
        ::close(fd);
  }

  Result<void>
  Initialize() {
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
      return tl::make_unexpected(Error{ErrorCode::kPlatform,
        "Interactive mode requires a terminal console; use manbo search or "
        "read"});
    if (tcgetattr(STDIN_FILENO, &original_) < 0)
      return tl::make_unexpected(PlatformError("tcgetattr"));
    termios raw = original_;
    raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO | IEXTEN));
    raw.c_iflag &=
      static_cast<tcflag_t>(~(IXON | ICRNL | BRKINT | INPCK | ISTRIP));
    raw.c_cflag |= CS8;
    raw.c_oflag &= static_cast<tcflag_t>(~OPOST);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) < 0)
      return tl::make_unexpected(PlatformError("tcsetattr"));
    termios_changed_ = true;
    if (pipe2(resize_pipe_, O_CLOEXEC | O_NONBLOCK) < 0)
      return tl::make_unexpected(PlatformError("pipe2"));
    resize_fd = resize_pipe_[1];
    struct sigaction action{};
    action.sa_handler = OnResize;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    if (sigaction(SIGWINCH, &action, &old_action_) < 0)
      return tl::make_unexpected(PlatformError("sigaction"));
    signal_changed_ = true;
    active_ = true;
    constexpr std::string_view enter = "\x1b[?1049h\x1b[?25l\x1b[2J\x1b[H";
    return Write(enter);
  }

  Result<ConsoleEvent>
  Next(WakeSignal& wake, bool prefer_wake) override {
    if (!records_.empty()) {
      auto item = records_.front();
      records_.pop_front();
      return item;
    }
    std::array<pollfd, 3> fds{{{STDIN_FILENO, POLLIN, 0},
      {wake.impl_->fd, POLLIN, 0},
      {resize_pipe_[0], POLLIN, 0}}};
    if (prefer_wake)
      std::swap(fds[0], fds[1]);
    ++metrics_.waits;
    int result;
    do {
      result = ::poll(fds.data(), fds.size(), -1);
    } while (result < 0 && errno == EINTR);
    if (result < 0)
      return tl::make_unexpected(PlatformError("poll"));
    for (const auto& fd : fds) {
      if (!(fd.revents & POLLIN))
        continue;
      if (fd.fd == wake.impl_->fd)
        return ConsoleEvent{};
      if (fd.fd == resize_pipe_[0]) {
        char bytes[32];
        (void)::read(fd.fd, bytes, sizeof(bytes));
        return ConsoleEvent{ConsoleEventKind::kResize};
      }
      unsigned char c;
      const auto n = ::read(STDIN_FILENO, &c, 1);
      if (n <= 0)
        return tl::make_unexpected(
          Error{ErrorCode::kPlatform, "Terminal input closed"});
      ++metrics_.input_records;
      return Decode(c);
    }
    return ConsoleEvent{};
  }

  Result<ConsoleSize>
  Size() const override {
    winsize size{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) < 0)
      return tl::make_unexpected(PlatformError("ioctl(TIOCGWINSZ)"));
    return ConsoleSize{size.ws_col, size.ws_row};
  }

  Result<void>
  Write(std::string_view bytes) override {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
      auto n =
        ::write(STDOUT_FILENO, bytes.data() + offset, bytes.size() - offset);
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0)
        return tl::make_unexpected(PlatformError("write"));
      offset += static_cast<std::size_t>(n);
    }
    return {};
  }

  ConsoleMetrics
  metrics() const noexcept override {
    return metrics_;
  }

 private:
  ConsoleEvent
  Decode(unsigned char c) {
    ConsoleEvent e;
    e.kind = ConsoleEventKind::kKey;
    if (c == 0x1b) {
      pollfd ready{STDIN_FILENO, POLLIN, 0};
      if (::poll(&ready, 1, 25) <= 0) {
        e.key = ConsoleKey::kEscape;
        return e;
      }
      char sequence[5]{};
      const auto count = ::read(STDIN_FILENO, sequence, 1);
      if (count != 1 || sequence[0] != '[') {
        e.key = ConsoleKey::kEscape;
        if (count == 1)
          records_.push_back(Character(sequence[0]));
        return e;
      }
      if (::read(STDIN_FILENO, sequence + 1, 1) != 1) {
        e.key = ConsoleKey::kEscape;
        return e;
      }
      e.key = ConsoleKey::kCharacter;
      switch (sequence[1]) {
        case 'A':
          e.key = ConsoleKey::kUp;
          break;
        case 'B':
          e.key = ConsoleKey::kDown;
          break;
        case 'Z':
          e.key = ConsoleKey::kBackTab;
          break;
        case '5':
          if (::read(STDIN_FILENO, sequence + 2, 1) == 1 && sequence[2] == '~')
            e.key = ConsoleKey::kPageUp;
          break;
        case '6':
          if (::read(STDIN_FILENO, sequence + 2, 1) == 1 && sequence[2] == '~')
            e.key = ConsoleKey::kPageDown;
          break;
        default:
          break;
      }
      return e;
    }
    if (c == '\r' || c == '\n') {
      e.key = ConsoleKey::kEnter;
      return e;
    }
    if (c == 0x7f || c == 0x08) {
      e.key = ConsoleKey::kBackspace;
      return e;
    }
    if (c == '\t') {
      e.key = ConsoleKey::kTab;
      return e;
    }
    if (c < 0x20) {
      e.key = ConsoleKey::kCharacter;
      e.control = true;
      e.shortcut = c >= 1 && c <= 26 ? static_cast<char>('A' + c - 1) : 0;
      return e;
    }
    e.text.assign(1, static_cast<char>(c));
    // Preserve valid UTF-8 input by collecting continuation bytes in the next
    // poll calls.
    int trailing = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC2 ? 1 : 0;
    while (trailing-- > 0) {
      unsigned char next;
      if (::read(STDIN_FILENO, &next, 1) != 1 || (next & 0xC0) != 0x80)
        break;
      e.text.push_back(static_cast<char>(next));
    }
    if (c >= 'A' && c <= 'Z')
      e.shortcut = static_cast<char>(c);
    return e;
  }

  ConsoleEvent
  Character(char c) {
    ConsoleEvent e;
    e.kind = ConsoleEventKind::kKey;
    e.text.assign(1, c);
    return e;
  }
  termios original_{};
  bool termios_changed_ = false, active_ = false, signal_changed_ = false;
  int resize_pipe_[2]{-1, -1};
  struct sigaction old_action_{};
  std::deque<ConsoleEvent> records_;
  ConsoleMetrics metrics_;
};

bool
ConsoleAvailable() noexcept {
  return isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
}

Result<std::unique_ptr<IConsole>>
OpenConsole() {
  auto console = std::make_unique<NativeConsole>();
  auto ready = console->Initialize();
  if (!ready)
    return tl::make_unexpected(ready.error());
  return std::unique_ptr<IConsole>(std::move(console));
}
}  // namespace manbo
