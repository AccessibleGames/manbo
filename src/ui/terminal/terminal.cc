#include "ui/terminal/terminal.h"

#include <fmt/format.h>

#include <algorithm>
#include <limits>
#include <utility>

#include "core/command.h"
#include "platform/console.h"
#include "ui/terminal/view.h"

namespace manbo {
namespace {
auto
EraseLastCharacter(std::string& text) -> void {
  if (text.empty())
    return;
  auto offset = text.size() - 1;
  while (
    offset > 0 && (static_cast<unsigned char>(text[offset]) & 0xc0) == 0x80)
    --offset;
  text.resize(offset);
}
class TerminalFrontend final : public ITerminal {
 public:
  TerminalFrontend(EventChannel& replies, std::unique_ptr<IConsole> console)
      : replies_(replies), console_(std::move(console)) {
  }

  auto
  Next() -> Result<std::optional<Event>> override {
    while (true) {
      if (prefer_reply_)
        if (auto worker_reply = replies_.TryReceive()) {
          prefer_reply_ = false;
          return worker_reply;
        }
      if (replies_.closed()) {
        // Close preserves buffered replies; drain after observing closure.
        if (auto worker_reply = replies_.TryReceive())
          return worker_reply;
        return std::optional<Event>{};
      }
      auto console_input =
        console_->Next(replies_.wake_signal(), prefer_reply_);
      if (!console_input)
        return tl::make_unexpected(console_input.error());
      if (console_input->kind == ConsoleEventKind::kWake) {
        if (auto worker_reply = replies_.TryReceive()) {
          prefer_reply_ = false;
          return worker_reply;
        }
      } else if (auto translated_event = Translate(*console_input)) {
        prefer_reply_ = true;
        return translated_event;
      }
    }
  }

  auto
  Render(const Snapshot& snapshot) -> Result<void> override {
    auto terminal_size = console_->Size();
    if (!terminal_size)
      return tl::make_unexpected(terminal_size.error());
    if (snapshot.search_home && !state_.search_home) {
      prompt_.mode = InputMode::kQuery;
      prompt_.text.clear();
      prompt_.notice.clear();
    }

    const bool document_changed =
      state_.document.has_value() != snapshot.document.has_value() ||
      (state_.document && snapshot.document &&
        (state_.document->id != snapshot.document->id ||
          state_.document->content != snapshot.document->content ||
          state_.document->body != snapshot.document->body));
    if (document_changed) {
      if (prompt_.mode == InputMode::kNavigation)
        prompt_.notice.clear();
      prompt_.find_query.clear();
      prompt_.current_match.reset();
      active_match_.reset();
      matches_.clear();
      prompt_.selected_link.clear();
    }
    if (width_ != terminal_size->width) {
      active_match_.reset();
      prompt_.current_match.reset();
    }
    width_ = std::clamp(terminal_size->width, 1, 240);
    height_ = std::clamp(terminal_size->height, 1, 100);
    state_ = snapshot;
    auto rendered_frame = RenderFrame(snapshot, width_, height_, prompt_);
    reader_scroll_limit_ = rendered_frame.reader_scroll_limit;
    reader_offset_ = rendered_frame.reader_offset;
    links_ = std::move(rendered_frame.reader_links);
    reader_height_ = std::max(1, rendered_frame.reader_content_height);
    matches_ = std::move(rendered_frame.find_matches);
    pending_frame_ = SerializeAnsi(rendered_frame);
    return {};
  }

  auto
  Flush() -> Result<void> override {
    auto write_result = console_->Write(pending_frame_);
    if (write_result) {
      frame_bytes_ += pending_frame_.size();
      pending_frame_.clear();
    }
    return write_result;
  }

  auto
  metrics() const noexcept -> TerminalMetrics override {
    const auto native = console_->metrics();
    return {frame_bytes_, native.waits, native.input_records};
  }

 private:
  auto
  CancelPrompt() -> Event {
    prompt_.mode = InputMode::kNavigation;
    prompt_.text.clear();
    prompt_.notice.clear();
    return Refresh{};
  }

  auto
  Begin(InputMode mode, std::string text = {}) -> Event {
    prompt_.selected_link.clear();
    prompt_.mode = mode;
    prompt_.text = std::move(text);
    prompt_.notice.clear();
    return Refresh{};
  }

  auto
  Notice(std::string text) -> Event {
    prompt_.notice = std::move(text);
    return Refresh{};
  }

  auto
  Find(int search_direction, bool start_from_first_match = false) -> Event {
    prompt_.selected_link.clear();
    if (!state_.document || prompt_.find_query.empty())
      return Notice("No previous in-page search");
    if (matches_.empty())
      return Notice("Pattern not found: " + prompt_.find_query);
    bool wrapped = false;
    std::size_t match_index = 0;
    if (!start_from_first_match && active_match_) {
      if (search_direction > 0) {
        match_index = (*active_match_ + 1) % matches_.size();
        wrapped = match_index == 0;
      } else {
        match_index =
          *active_match_ == 0 ? matches_.size() - 1 : *active_match_ - 1;
        wrapped = *active_match_ == 0;
      }
    } else {
      const auto visible_line = reader_offset_;
      const auto match =
        std::ranges::lower_bound(matches_, visible_line, {}, &TextMatch::line);
      match_index = match == matches_.end()
                      ? 0
                      : static_cast<std::size_t>(match - matches_.begin());
      wrapped = match == matches_.end();
      if (search_direction < 0) {
        wrapped = match_index == 0;
        match_index = match_index == 0 ? matches_.size() - 1 : match_index - 1;
      }
    }
    active_match_ = match_index;
    prompt_.current_match = matches_[match_index];
    prompt_.notice = fmt::format("/{}  {}/{}{}",
      prompt_.find_query,
      match_index + 1,
      matches_.size(),
      wrapped ? " (wrapped)" : "");
    return ScrollTo{matches_[match_index].line, reader_scroll_limit_};
  }

  auto
  SubmitPrompt() -> Event {
    const auto input_mode = prompt_.mode;
    auto submitted_text = std::move(prompt_.text);
    CancelPrompt();
    if (input_mode == InputMode::kCommand) {
      auto parsed_command = ParseCommand(submitted_text);
      if (!parsed_command)
        return Notice(parsed_command.error().message);
      if (state_.route == Route::kResults &&
          (std::holds_alternative<Back>(*parsed_command) ||
            (std::holds_alternative<Search>(*parsed_command) &&
              !state_.search_home &&
              std::get<Search>(*parsed_command).query == state_.query)))
        return Refresh{};
      return std::move(*parsed_command);
    }
    if (input_mode == InputMode::kQuery) {
      // Re-submitting the current query still dismisses the editor visibly.
      if (state_.route == Route::kResults && !state_.search_home &&
          submitted_text == state_.query)
        return Refresh{};
      return Search{std::move(submitted_text)};
    }
    if (!submitted_text.empty())
      prompt_.find_query = std::move(submitted_text);
    matches_ = state_.document ? FindDocumentText(
                                   *state_.document, width_, prompt_.find_query)
                               : std::vector<TextMatch>{};
    active_match_.reset();
    prompt_.current_match.reset();
    return Find(1, true);
  }

  auto
  Move(int lines) -> Event {
    const bool clear_selection = !prompt_.selected_link.empty();
    prompt_.selected_link.clear();
    prompt_.notice.clear();
    if (state_.route == Route::kReader && clear_selection &&
        ((lines < 0 && reader_offset_ == 0) ||
          (lines >= 0 && reader_offset_ == reader_scroll_limit_)))
      return Refresh{};
    return state_.route == Route::kReader
             ? Event{Scroll{lines, reader_scroll_limit_, reader_offset_}}
             : Event{Select{lines}};
  }

  Event
  SelectLink(int direction) {
    if (state_.route != Route::kReader || !state_.document || state_.pending)
      return Refresh{};
    if (links_.empty())
      return Notice("This page has no references");

    const auto selected =
      std::ranges::find(links_, prompt_.selected_link, &PageLink::target);
    std::size_t index;
    if (selected != links_.end()) {
      index = static_cast<std::size_t>(selected - links_.begin());
      index = direction > 0 ? (index + 1) % links_.size()
              : index == 0  ? links_.size() - 1
                            : index - 1;
    } else {
      const auto first_visible =
        std::ranges::lower_bound(links_, reader_offset_, {}, &PageLink::line);
      index = first_visible == links_.end()
                ? 0
                : static_cast<std::size_t>(first_visible - links_.begin());
      if (direction < 0)
        index = index == 0 ? links_.size() - 1 : index - 1;
    }
    const auto& link = links_[index];
    prompt_.selected_link = link.target;
    prompt_.notice.clear();
    if (link.line >= reader_offset_ &&
        link.line < reader_offset_ + static_cast<std::size_t>(reader_height_))
      return Refresh{};
    return ScrollTo{link.line, reader_scroll_limit_};
  }

  auto
  Translate(const ConsoleEvent& console_input) -> std::optional<Event> {
    if (console_input.kind == ConsoleEventKind::kResize)
      return Refresh{};
    if (console_input.kind == ConsoleEventKind::kWheel) {
      if (prompt_.mode != InputMode::kNavigation)
        return std::nullopt;
      const auto steps = std::clamp(console_input.wheel_steps, -256, 256);
      return Move(state_.route == Route::kReader ? -3 * steps : -steps);
    }
    if (console_input.kind != ConsoleEventKind::kKey)
      return std::nullopt;
    const bool is_control_shortcut =
      console_input.control && !console_input.alt;
    if (is_control_shortcut && console_input.shortcut == 'Q')
      return Quit{};
    if (is_control_shortcut && console_input.shortcut == 'C')
      return prompt_.mode == InputMode::kNavigation ? Event{Quit{}}
                                                    : CancelPrompt();
    if (is_control_shortcut && console_input.shortcut == 'L')
      return Refresh{};
    const int repeat_count = std::clamp(console_input.repeat, 1, 256);
    if (prompt_.mode != InputMode::kNavigation) {
      if (console_input.key == ConsoleKey::kEscape)
        return CancelPrompt();
      if (console_input.key == ConsoleKey::kEnter)
        return SubmitPrompt();
      if (is_control_shortcut && console_input.shortcut == 'U') {
        prompt_.text.clear();
        return Refresh{};
      }
      if (console_input.key == ConsoleKey::kBackspace) {
        for (int repeat_index = 0; repeat_index < repeat_count; ++repeat_index)
          EraseLastCharacter(prompt_.text);
        return Refresh{};
      }
      if (console_input.key != ConsoleKey::kCharacter ||
          (is_control_shortcut && !console_input.alt))
        return std::nullopt;
      if (console_input.text.empty() ||
          static_cast<unsigned char>(console_input.text.front()) < 0x20 ||
          console_input.text.front() == 0x7f)
        return std::nullopt;
      const auto safe_input_text = SafeText(console_input.text);
      const std::size_t maximum_prompt_bytes =
        prompt_.mode == InputMode::kCommand ? 512 : 256;
      for (int repeat_index = 0;
        repeat_index < repeat_count &&
        prompt_.text.size() + safe_input_text.size() <= maximum_prompt_bytes;
        ++repeat_index)
        prompt_.text += safe_input_text;
      return Refresh{};
    }
    const auto page_step = state_.route == Route::kReader
                             ? reader_height_
                             : std::max(1, (height_ - 6) / 3);
    switch (console_input.key) {
      case ConsoleKey::kEscape:
        prompt_.selected_link.clear();
        prompt_.notice.clear();
        return state_.route == Route::kReader ? Event{Back{}} : Event{Quit{}};
      case ConsoleKey::kEnter:
        if (state_.route == Route::kReader && !prompt_.selected_link.empty()) {
          auto target = std::move(prompt_.selected_link);
          prompt_.selected_link.clear();
          return FollowLink{std::move(target)};
        }
        return state_.route == Route::kReader ? Move(repeat_count)
                                              : Event{OpenSelected{}};
      case ConsoleKey::kTab:
        return SelectLink(1);
      case ConsoleKey::kBackTab:
        return SelectLink(-1);
      case ConsoleKey::kUp:
        return Move(-repeat_count);
      case ConsoleKey::kDown:
        return Move(repeat_count);
      case ConsoleKey::kPageUp:
        return Move(-page_step * repeat_count);
      case ConsoleKey::kPageDown:
        return Move(page_step * repeat_count);
      case ConsoleKey::kBackspace: {
        if (state_.route != Route::kResults)
          return std::nullopt;
        auto query = state_.query;
        for (int repeat_index = 0; repeat_index < repeat_count; ++repeat_index)
          EraseLastCharacter(query);
        return Begin(InputMode::kQuery, std::move(query));
      }
      case ConsoleKey::kCharacter:
        break;
    }
    if (console_input.text.empty() || is_control_shortcut ||
        static_cast<unsigned char>(console_input.text.front()) < 0x20 ||
        console_input.text.front() == 0x7f)
      return std::nullopt;
    if (console_input.text == ":")
      return Begin(InputMode::kCommand);
    if (console_input.text == "/")
      return Begin(
        state_.route == Route::kReader ? InputMode::kFind : InputMode::kQuery);
    if (console_input.text == "q")
      return Quit{};
    if (console_input.text == "?")
      return OpenPage{"manbo(1)"};
    if (console_input.text == "j")
      return Move(repeat_count);
    if (console_input.text == "k")
      return Move(-repeat_count);
    if (console_input.text == " ")
      return Move(page_step * repeat_count);
    if (console_input.text == "b")
      return Move(-page_step * repeat_count);
    if (console_input.text == "g" || console_input.text == "G") {
      prompt_.selected_link.clear();
      prompt_.notice.clear();
      if (state_.route == Route::kReader)
        return ScrollTo{console_input.text == "g" ? 0 : reader_scroll_limit_,
          reader_scroll_limit_};
      return Select{console_input.text == "g"
                      ? -std::numeric_limits<int>::max()
                      : std::numeric_limits<int>::max()};
    }
    if (state_.route == Route::kReader &&
        (console_input.text == "n" || console_input.text == "N"))
      return Find(console_input.text == "n" ? 1 : -1);
    if (state_.route == Route::kResults) {
      std::string query_text;
      const auto safe_input_text = SafeText(console_input.text);
      for (int repeat_index = 0;
        repeat_index < repeat_count &&
        query_text.size() + safe_input_text.size() <= 256;
        ++repeat_index)
        query_text += safe_input_text;
      return Begin(InputMode::kQuery, std::move(query_text));
    }
    return std::nullopt;
  }
  EventChannel& replies_;
  std::unique_ptr<IConsole> console_;
  bool prefer_reply_ = true;
  int width_ = 80;
  int height_ = 24;
  int reader_height_ = 21;
  std::size_t reader_scroll_limit_ = 0;
  std::size_t reader_offset_ = 0;
  std::uint64_t frame_bytes_ = 0;
  Snapshot state_;
  TerminalPrompt prompt_;
  std::vector<TextMatch> matches_;
  std::vector<PageLink> links_;
  std::optional<std::size_t> active_match_;
  std::string pending_frame_;
};
}  // namespace

auto
TerminalAvailable() noexcept -> bool {
  return ConsoleAvailable();
}

auto
MakeTerminal(EventChannel& replies, std::unique_ptr<IConsole> console)
  -> Result<std::unique_ptr<ITerminal>> {
  if (!console)
    return tl::make_unexpected(Error{ErrorCode::kInvalidArgument,
      "A terminal frontend requires a console transport"});
  return std::unique_ptr<ITerminal>(
    std::make_unique<TerminalFrontend>(replies, std::move(console)));
}

auto
MakeTerminal(EventChannel& replies) -> Result<std::unique_ptr<ITerminal>> {
  auto console = OpenConsole();
  if (!console)
    return tl::make_unexpected(console.error());
  return MakeTerminal(replies, std::move(*console));
}
}  // namespace manbo
