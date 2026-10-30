// Windows native headers stay isolated in this platform adapter.
#include <fmt/format.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <deque>
#include <optional>
#include <utility>

#include "platform/console.h"

namespace manbo {
namespace {
Error
PlatformError(std::string_view operation) {
  return {ErrorCode::kPlatform,
    fmt::format("{} failed (Windows error {})", operation, GetLastError())};
}

std::wstring
ToWide(std::string_view text) {
  if (text.empty())
    return {};
  const int count = MultiByteToWideChar(CP_UTF8,
    MB_ERR_INVALID_CHARS,
    text.data(),
    static_cast<int>(text.size()),
    nullptr,
    0);
  if (count <= 0)
    return {};
  std::wstring wide(static_cast<std::size_t>(count), L'\0');
  MultiByteToWideChar(CP_UTF8,
    MB_ERR_INVALID_CHARS,
    text.data(),
    static_cast<int>(text.size()),
    wide.data(),
    count);
  return wide;
}

std::string
ToUtf8(std::wstring_view text) {
  const int count = WideCharToMultiByte(CP_UTF8,
    WC_ERR_INVALID_CHARS,
    text.data(),
    static_cast<int>(text.size()),
    nullptr,
    0,
    nullptr,
    nullptr);
  if (count <= 0)
    return {};
  std::string result(static_cast<std::size_t>(count), '\0');
  WideCharToMultiByte(CP_UTF8,
    WC_ERR_INVALID_CHARS,
    text.data(),
    static_cast<int>(text.size()),
    result.data(),
    count,
    nullptr,
    nullptr);
  return result;
}

Result<void>
WriteText(HANDLE output, std::string_view text) {
  auto wide = ToWide(text);
  if (!text.empty() && wide.empty())
    return tl::make_unexpected(PlatformError("UTF-8 conversion"));
  std::size_t offset = 0;
  while (offset < wide.size()) {
    DWORD written = 0;
    if (!WriteConsoleW(output,
          wide.data() + offset,
          static_cast<DWORD>(wide.size() - offset),
          &written,
          nullptr) ||
        written == 0)
      return tl::make_unexpected(PlatformError("WriteConsoleW"));
    offset += written;
  }
  return {};
}

ConsoleKey
NormalizeKey(WORD key) {
  switch (key) {
    case VK_ESCAPE:
      return ConsoleKey::kEscape;
    case VK_RETURN:
      return ConsoleKey::kEnter;
    case VK_UP:
      return ConsoleKey::kUp;
    case VK_DOWN:
      return ConsoleKey::kDown;
    case VK_PRIOR:
      return ConsoleKey::kPageUp;
    case VK_NEXT:
      return ConsoleKey::kPageDown;
    case VK_BACK:
      return ConsoleKey::kBackspace;
    case VK_TAB:
      return ConsoleKey::kTab;
    default:
      return ConsoleKey::kCharacter;
  }
}
}  // namespace

struct WakeSignal::Impl {
  HANDLE event = nullptr;
  ~Impl() {
    if (event)
      CloseHandle(event);
  }
};
WakeSignal::WakeSignal(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {
}
WakeSignal::~WakeSignal() = default;
Result<std::unique_ptr<WakeSignal>>
WakeSignal::Create() {
  auto impl = std::make_unique<Impl>();
  impl->event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!impl->event)
    return tl::make_unexpected(PlatformError("CreateEventW"));
  return std::unique_ptr<WakeSignal>(new WakeSignal(std::move(impl)));
}
void
WakeSignal::Notify() noexcept {
  SetEvent(impl_->event);
}
void
WakeSignal::Reset() noexcept {
  ResetEvent(impl_->event);
}

// Native transport only. UI routes, key bindings, views and frame generation
// live in ui/terminal, not in this OS wrapper.
class NativeConsole final : public IConsole {
 public:
  ~NativeConsole() override {
    if (alternate_) {
      auto ignored = WriteText(output_, "\x1b[0m\x1b[?25h\x1b[?1049l");
      (void)ignored;
    }
    if (cursor_saved_)
      SetConsoleCursorInfo(output_, &cursor_info_);
    if (output_changed_)
      SetConsoleMode(output_, output_mode_);
    if (input_changed_)
      SetConsoleMode(input_, input_mode_);
  }

  Result<void>
  Initialize() {
    input_ = GetStdHandle(STD_INPUT_HANDLE);
    output_ = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!GetConsoleMode(input_, &input_mode_) ||
        !GetConsoleMode(output_, &output_mode_))
      return tl::make_unexpected(Error{ErrorCode::kPlatform,
        "Interactive mode requires a Windows "
        "console; use manbo search or read"});
    cursor_saved_ = GetConsoleCursorInfo(output_, &cursor_info_) != FALSE;
    const DWORD input_mode = (input_mode_ | ENABLE_EXTENDED_FLAGS |
                               ENABLE_WINDOW_INPUT | ENABLE_MOUSE_INPUT) &
                             ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT |
                               ENABLE_PROCESSED_INPUT | ENABLE_QUICK_EDIT_MODE);
    if (!SetConsoleMode(input_, input_mode))
      return tl::make_unexpected(PlatformError("SetConsoleMode(input)"));
    input_changed_ = true;
    if (!SetConsoleMode(output_,
          output_mode_ | ENABLE_PROCESSED_OUTPUT |
            ENABLE_VIRTUAL_TERMINAL_PROCESSING))
      return tl::make_unexpected(PlatformError("SetConsoleMode(output)"));
    output_changed_ = true;
    alternate_ = true;
    return WriteText(output_, "\x1b[?1049h\x1b[?25l\x1b[2J\x1b[H");
  }

  Result<ConsoleEvent>
  Next(WakeSignal& wake, bool prefer_wake) override {
    while (true) {
      while (!records_.empty()) {
        auto record = records_.front();
        records_.pop_front();
        if (auto event = Translate(record))
          return *event;
      }
      const HANDLE signal = wake.impl_->event;
      const std::array<HANDLE, 2> handles =
        prefer_wake ? std::array<HANDLE, 2>{signal, input_}
                    : std::array<HANDLE, 2>{input_, signal};
      ++metrics_.waits;
      const DWORD ready = WaitForMultipleObjects(
        static_cast<DWORD>(handles.size()), handles.data(), FALSE, INFINITE);
      if (ready >= WAIT_OBJECT_0 + handles.size())
        return tl::make_unexpected(PlatformError("WaitForMultipleObjects"));
      if (handles[ready - WAIT_OBJECT_0] == signal)
        return ConsoleEvent{};
      std::array<INPUT_RECORD, 32> records{};
      DWORD read = 0;
      if (!ReadConsoleInputW(
            input_, records.data(), static_cast<DWORD>(records.size()), &read))
        return tl::make_unexpected(PlatformError("ReadConsoleInputW"));
      metrics_.input_records += read;
      for (DWORD n = 0; n < read; ++n)
        records_.push_back(records[n]);
    }
  }

  Result<ConsoleSize>
  Size() const override {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (!GetConsoleScreenBufferInfo(output_, &info))
      return tl::make_unexpected(PlatformError("GetConsoleScreenBufferInfo"));
    return ConsoleSize{info.srWindow.Right - info.srWindow.Left + 1,
      info.srWindow.Bottom - info.srWindow.Top + 1};
  }

  Result<void>
  Write(std::string_view bytes) override {
    return WriteText(output_, bytes);
  }

  ConsoleMetrics
  metrics() const noexcept override {
    return metrics_;
  }

 private:
  std::optional<ConsoleEvent>
  Translate(const INPUT_RECORD& record) {
    ConsoleEvent event;
    if (record.EventType == WINDOW_BUFFER_SIZE_EVENT) {
      event.kind = ConsoleEventKind::kResize;
      return event;
    }
    if (record.EventType == MOUSE_EVENT) {
      if (record.Event.MouseEvent.dwEventFlags != MOUSE_WHEELED)
        return std::nullopt;
      const auto delta =
        static_cast<short>(HIWORD(record.Event.MouseEvent.dwButtonState));
      event.kind = ConsoleEventKind::kWheel;
      event.wheel_steps = delta > 0 ? 1 : -1;
      return event;
    }
    if (record.EventType != KEY_EVENT || !record.Event.KeyEvent.bKeyDown)
      return std::nullopt;
    const auto& key = record.Event.KeyEvent;
    event.kind = ConsoleEventKind::kKey;
    event.key = NormalizeKey(key.wVirtualKeyCode);
    if (event.key == ConsoleKey::kTab &&
        (key.dwControlKeyState & SHIFT_PRESSED) != 0)
      event.key = ConsoleKey::kBackTab;
    event.control =
      (key.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
    event.alt =
      (key.dwControlKeyState & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0;
    event.repeat = std::clamp(static_cast<int>(key.wRepeatCount), 1, 256);
    if (key.wVirtualKeyCode >= 'A' && key.wVirtualKeyCode <= 'Z')
      event.shortcut = static_cast<char>(key.wVirtualKeyCode);
    if (event.key != ConsoleKey::kCharacter) {
      high_surrogate_ = 0;
      return event;
    }
    const wchar_t character = key.uChar.UnicodeChar;
    if (character >= 0xd800 && character <= 0xdbff) {
      high_surrogate_ = character;
      return std::nullopt;
    }
    std::wstring characters;
    if (high_surrogate_) {
      characters += high_surrogate_;
      high_surrogate_ = 0;
    }
    characters += character;
    event.text = ToUtf8(characters);
    return event;
  }
  HANDLE input_ = nullptr;
  HANDLE output_ = nullptr;
  DWORD input_mode_ = 0;
  DWORD output_mode_ = 0;
  CONSOLE_CURSOR_INFO cursor_info_{};
  bool cursor_saved_ = false;
  bool input_changed_ = false;
  bool output_changed_ = false;
  bool alternate_ = false;
  wchar_t high_surrogate_ = 0;
  std::deque<INPUT_RECORD> records_;
  ConsoleMetrics metrics_;
};

bool
ConsoleAvailable() noexcept {
  DWORD mode = 0;
  return GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode) &&
         GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode);
}

Result<std::unique_ptr<IConsole>>
OpenConsole() {
  auto console = std::make_unique<NativeConsole>();
  auto initialized = console->Initialize();
  if (!initialized)
    return tl::make_unexpected(initialized.error());
  return std::unique_ptr<IConsole>(std::move(console));
}

}  // namespace manbo
