#include "ui/terminal/terminal.h"

#include <gtest/gtest.h>

#include <algorithm>

#include "platform/console.h"
#include "repositories/manual_repository.h"
#include "repositories/sample_repository.h"
#include "runtime/jobs.h"
#include "runtime/session.h"
#include "support/fixtures.h"
#include "ui/terminal/view.h"

namespace manbo {
namespace {
class ScriptConsole final : public IConsole {
 public:
  auto
  Next(WakeSignal&, bool) -> Result<ConsoleEvent> override {
    ++calls;
    if (position == input.size())
      return tl::make_unexpected(
        Error{ErrorCode::kPlatform, "Script exhausted"});
    return input[position++];
  }

  auto
  Size() const -> Result<ConsoleSize> override {
    return viewport;
  }

  auto
  Write(std::string_view text) -> Result<void> override {
    if (fail_write)
      return tl::make_unexpected(
        Error{ErrorCode::kPlatform, "Script write failed"});
    output += text;
    ++writes;
    return {};
  }

  auto
  metrics() const noexcept -> ConsoleMetrics override {
    return {calls, calls};
  }
  ConsoleSize viewport{80, 24};
  std::vector<ConsoleEvent> input;
  std::string output;
  std::size_t position = 0;
  std::uint64_t calls = 0;
  int writes = 0;
  bool fail_write = false;
};
auto
Key(ConsoleKey key, std::string text = {}) -> ConsoleEvent {
  ConsoleEvent event;
  event.kind = ConsoleEventKind::kKey;
  event.key = key;
  event.text = std::move(text);
  return event;
}

auto
Text(std::string text) -> ConsoleEvent {
  return Key(ConsoleKey::kCharacter, std::move(text));
}

auto
Control(char shortcut) -> ConsoleEvent {
  auto event = Text("");
  event.control = true;
  event.shortcut = shortcut;
  return event;
}

auto
Next(ITerminal& terminal) -> Event {
  auto event = terminal.Next();
  EXPECT_TRUE(event && *event);
  return event && *event ? std::move(**event) : Event{Quit{}};
}

auto
Reader() -> Snapshot {
  std::string text = "alpha ALPHA\n";
  for (int n = 0; n < 50; ++n)
    text += "A line\n";
  text += "alpha\n";
  auto content = std::make_shared<RichDocument>();
  content->blocks.push_back({BlockKind::kPreformatted, {{text}}});
  Snapshot state;
  state.route = Route::kReader;
  state.document = Document{"test/page",
    "Page",
    "",
    "",
    text,
    "Test",
    SourceFormat::kPlainText,
    content};
  return state;
}

TEST(TerminalFrontend, SearchHomeStartsEditingAndSubmitsEmptyOrTypedQueries) {
  for (const std::string query : {"", "socket"}) {
    auto channel = EventChannel::Create();
    ASSERT_TRUE(channel);
    auto console = std::make_unique<ScriptConsole>();
    auto* transport = console.get();
    if (!query.empty())
      console->input.push_back(Text(query));
    console->input.push_back(Key(ConsoleKey::kEnter));
    auto terminal = MakeTerminal(**channel, std::move(console));
    ASSERT_TRUE(terminal);
    Snapshot home;
    home.search_home = true;
    ASSERT_TRUE((*terminal)->Render(home));
    ASSERT_TRUE((*terminal)->Flush());
    EXPECT_NE(transport->output.find("Search: "), std::string::npos);
    EXPECT_NE(
      transport->output.find("Search your documentation"), std::string::npos);
    EXPECT_EQ(transport->output.find("No matching pages"), std::string::npos);
    if (!query.empty()) {
      EXPECT_TRUE(std::holds_alternative<Refresh>(Next(**terminal)));
      ASSERT_TRUE((*terminal)->Render(home));
    }
    const auto submitted = Next(**terminal);
    ASSERT_TRUE(std::holds_alternative<Search>(submitted));
    EXPECT_EQ(std::get<Search>(submitted).query, query);
  }
}

TEST(TerminalFrontend, FrameGenerationDoesNotWriteUntilFlush) {
  auto channel = EventChannel::Create();
  ASSERT_TRUE(channel);
  EXPECT_FALSE(MakeTerminal(**channel, std::unique_ptr<IConsole>{}));
  auto console = std::make_unique<ScriptConsole>();
  auto* transport = console.get();
  auto terminal = MakeTerminal(**channel, std::move(console));
  ASSERT_TRUE(terminal);
  Snapshot state;
  state.query = "example";
  ASSERT_TRUE((*terminal)->Render(state));
  EXPECT_TRUE(transport->output.empty());
  EXPECT_EQ((*terminal)->metrics().frame_bytes, 0u);
  ASSERT_TRUE((*terminal)->Flush());
  EXPECT_EQ(transport->output, SerializeAnsi(RenderFrame(state, 80, 24)));
  const auto bytes = (*terminal)->metrics().frame_bytes;
  transport->fail_write = true;
  ASSERT_TRUE((*terminal)->Render(state));
  EXPECT_FALSE((*terminal)->Flush());
  EXPECT_EQ((*terminal)->metrics().frame_bytes, bytes);
}

TEST(TerminalFrontend, NormalizedReaderKeysCarryRenderedScrollBounds) {
  auto channel = EventChannel::Create();
  ASSERT_TRUE(channel);
  auto console = std::make_unique<ScriptConsole>();
  auto down = Key(ConsoleKey::kDown);
  down.repeat = 2;
  console->input = {down, Key(ConsoleKey::kEscape)};
  auto terminal = MakeTerminal(**channel, std::move(console));
  ASSERT_TRUE(terminal);
  const auto state = Reader();
  ASSERT_TRUE((*terminal)->Render(state));
  const auto event = Next(**terminal);
  const auto& scroll = std::get<Scroll>(event);
  EXPECT_EQ(scroll.lines, 2);
  EXPECT_EQ(scroll.max_offset, RenderFrame(state, 80, 24).reader_scroll_limit);
  EXPECT_GT(*scroll.max_offset, 20u);
  EXPECT_TRUE(std::holds_alternative<Back>(Next(**terminal)));
}

TEST(TerminalFrontend, UnicodeEditingAndAltGrCommitOnlyOnEnter) {
  auto channel = EventChannel::Create();
  ASSERT_TRUE(channel);
  auto console = std::make_unique<ScriptConsole>();
  auto* transport = console.get();
  auto altgr = Text("@");
  altgr.control = true;
  altgr.alt = true;
  altgr.shortcut = 'Q';
  console->input = {
    Key(ConsoleKey::kBackspace), altgr, Key(ConsoleKey::kEnter), Control('Q')};
  auto terminal = MakeTerminal(**channel, std::move(console));
  ASSERT_TRUE(terminal);
  Snapshot state;
  state.query = "a中";
  ASSERT_TRUE((*terminal)->Render(state));
  EXPECT_TRUE(std::holds_alternative<Refresh>(Next(**terminal)));
  ASSERT_TRUE((*terminal)->Render(state));
  ASSERT_TRUE((*terminal)->Flush());
  EXPECT_NE(transport->output.find("Search: "), std::string::npos);
  EXPECT_TRUE(std::holds_alternative<Refresh>(Next(**terminal)));
  ASSERT_TRUE((*terminal)->Render(state));
  ASSERT_TRUE((*terminal)->Flush());
  EXPECT_NE(transport->output.find("a@"), std::string::npos);
  EXPECT_EQ(std::get<Search>(Next(**terminal)).query, "a@");
  EXPECT_TRUE(std::holds_alternative<Quit>(Next(**terminal)));
}

TEST(TerminalFrontend, QueuedRepliesDoNotNeedConsolePolling) {
  auto channel = EventChannel::Create();
  ASSERT_TRUE(channel);
  auto console = std::make_unique<ScriptConsole>();
  const auto* transport = console.get();
  auto terminal = MakeTerminal(**channel, std::move(console));
  ASSERT_TRUE(terminal);
  ASSERT_TRUE((*channel)->TrySend(Refresh{}));
  EXPECT_TRUE(std::holds_alternative<Refresh>(Next(**terminal)));
  EXPECT_EQ(transport->calls, 0u);
  (*channel)->Close();
  auto event = (*terminal)->Next();
  ASSERT_TRUE(event);
  EXPECT_FALSE(*event);
  EXPECT_EQ(transport->calls, 0u);
}

TEST(TerminalFrontend, ManLessPagingUsesViewportAndHasTopBottomAndQuit) {
  auto channel = EventChannel::Create();
  ASSERT_TRUE(channel);
  auto console = std::make_unique<ScriptConsole>();
  console->input = {Text("j"),
    Text("k"),
    Text(" "),
    Text("b"),
    Text("g"),
    Text("G"),
    Text("q")};
  auto terminal = MakeTerminal(**channel, std::move(console));
  ASSERT_TRUE(terminal);
  const auto state = Reader();
  ASSERT_TRUE((*terminal)->Render(state));
  EXPECT_EQ(std::get<Scroll>(Next(**terminal)).lines, 1);
  EXPECT_EQ(std::get<Scroll>(Next(**terminal)).lines, -1);
  EXPECT_EQ(std::get<Scroll>(Next(**terminal)).lines, 21);
  EXPECT_EQ(std::get<Scroll>(Next(**terminal)).lines, -21);
  EXPECT_EQ(std::get<ScrollTo>(Next(**terminal)).line, 0u);
  EXPECT_EQ(std::get<ScrollTo>(Next(**terminal)).line,
    RenderFrame(state, 80, 24).reader_scroll_limit);
  EXPECT_TRUE(std::holds_alternative<Quit>(Next(**terminal)));
}

TEST(
  TerminalFrontend, PromptCancellationAndShortcutTextNeverSubmitBackendWork) {
  auto channel = EventChannel::Create();
  ASSERT_TRUE(channel);
  auto console = std::make_unique<ScriptConsole>();
  console->input = {Text(":"),
    Text("qbn/N"),
    Control('U'),
    Text("man 3 printf"),
    Key(ConsoleKey::kEscape),
    Text(":"),
    Text("man 3 printf"),
    Key(ConsoleKey::kEnter)};
  auto terminal = MakeTerminal(**channel, std::move(console));
  ASSERT_TRUE(terminal);
  ASSERT_TRUE((*terminal)->Render(Reader()));
  for (int n = 0; n < 7; ++n)
    EXPECT_TRUE(std::holds_alternative<Refresh>(Next(**terminal))) << n;
  EXPECT_EQ(std::get<OpenPage>(Next(**terminal)).reference, "printf(3)");
}

TEST(TerminalFrontend, InvalidCommandsAndMissingPatternsStayLocalAndVisible) {
  auto channel = EventChannel::Create();
  ASSERT_TRUE(channel);
  auto console = std::make_unique<ScriptConsole>();
  auto* transport = console.get();
  console->input = {Text(":"),
    Text("not-a-command"),
    Key(ConsoleKey::kEnter),
    Text("/"),
    Text("missing-pattern"),
    Key(ConsoleKey::kEnter),
    Text("n"),
    Text("/"),
    Control('C'),
    Text("q")};
  auto terminal = MakeTerminal(**channel, std::move(console));
  ASSERT_TRUE(terminal);
  const auto state = Reader();
  ASSERT_TRUE((*terminal)->Render(state));
  for (int n = 0; n < 3; ++n)
    EXPECT_TRUE(std::holds_alternative<Refresh>(Next(**terminal)));
  ASSERT_TRUE((*terminal)->Render(state));
  ASSERT_TRUE((*terminal)->Flush());
  EXPECT_NE(transport->output.find("Unknown command:"), std::string::npos);
  for (int n = 0; n < 3; ++n)
    EXPECT_TRUE(std::holds_alternative<Refresh>(Next(**terminal)));
  ASSERT_TRUE((*terminal)->Render(state));
  ASSERT_TRUE((*terminal)->Flush());
  EXPECT_NE(transport->output.find("Pattern not found:"), std::string::npos);
  for (int n = 0; n < 3; ++n)
    EXPECT_TRUE(std::holds_alternative<Refresh>(Next(**terminal)));
  EXPECT_TRUE(std::holds_alternative<Quit>(Next(**terminal)));
}

TEST(TerminalFrontend, FindVisitsSameLineOccurrencesWrapsAndRebuildsOnResize) {
  auto channel = EventChannel::Create();
  ASSERT_TRUE(channel);
  auto console = std::make_unique<ScriptConsole>();
  auto* transport = console.get();
  console->input = {Text("/"),
    Text("alpha"),
    Key(ConsoleKey::kEnter),
    Text("n"),
    Text("n"),
    Text("n"),
    Text("N"),
    Text("n")};
  auto terminal = MakeTerminal(**channel, std::move(console));
  ASSERT_TRUE(terminal);
  auto state = Reader();
  const auto original = state.document->body;
  ASSERT_TRUE((*terminal)->Render(state));
  EXPECT_TRUE(std::holds_alternative<Refresh>(Next(**terminal)));
  EXPECT_TRUE(std::holds_alternative<Refresh>(Next(**terminal)));
  for (int n = 0; n < 5; ++n) {
    const auto seek = std::get<ScrollTo>(Next(**terminal));
    if (n < 2)
      EXPECT_EQ(seek.line, 0u);
    if (n == 2 || n == 4)
      EXPECT_GT(seek.line, 20u);
    state.scroll_line = std::min(seek.line, seek.max_offset);
    ASSERT_TRUE((*terminal)->Render(state));
    ASSERT_TRUE((*terminal)->Flush());
  }

  EXPECT_NE(transport->output.find("(wrapped)"), std::string::npos);
  transport->viewport = {40, 14};
  ASSERT_TRUE((*terminal)->Render(state));
  const auto resized = std::get<ScrollTo>(Next(**terminal));
  EXPECT_EQ(resized.max_offset, RenderFrame(state, 40, 14).reader_scroll_limit);
  EXPECT_EQ(state.document->body, original);
}

class AcknowledgedRepository final : public IDocumentRepository {
 public:
  explicit AcknowledgedRepository(ScriptConsole& console) : console_(console) {
  }

  auto
  Search(std::string_view query, std::stop_token stop)
    -> Result<std::vector<SearchHit>> override {
    return samples_.Search(query, stop);
  }

  auto
  Read(std::string_view reference, std::stop_token stop)
    -> Result<Document> override {
    if (reference == "printf(3)")
      EXPECT_NE(
        console_.output.find("Opening printf(3)..."), std::string::npos);
    return samples_.Read(reference, stop);
  }

 private:
  ScriptConsole& console_;
  SampleRepository samples_;
};

TEST(
  TerminalFrontend, CompleteCommandManualFindAndBackWorkflowFlushesBeforeRead) {
  auto channel = EventChannel::Create();
  ASSERT_TRUE(channel);
  auto console = std::make_unique<ScriptConsole>();
  auto* transport = console.get();
  console->input = {Text(":"),
    Text("man 3 printf"),
    Key(ConsoleKey::kEnter),
    Key(ConsoleKey::kEscape),
    Text(":"),
    Text("manbo manbo"),
    Key(ConsoleKey::kEnter),
    Text("/"),
    Text("COMMANDS"),
    Key(ConsoleKey::kEnter),
    Text("q")};
  auto repository =
    WithManboManual(std::make_unique<AcknowledgedRepository>(*transport));
  auto jobs = MakeJobService(*repository, **channel, 0);
  ASSERT_TRUE(jobs);
  auto terminal = MakeTerminal(**channel, std::move(console));
  ASSERT_TRUE(terminal);
  Session session(**terminal, **terminal, **jobs);
  auto result = session.Run("std::forward_list");
  ASSERT_TRUE(result) << (result ? "" : result.error().message);
  EXPECT_EQ(
    result->requests, 4u);  // Search, preview, printf(3), builtin manual.
  EXPECT_EQ(session.browser().state().document->id, "builtin/manbo/1");
  EXPECT_NE(transport->output.find("COMMANDS"), std::string::npos);
  EXPECT_NE(transport->output.find("/COMMANDS"), std::string::npos);
  EXPECT_EQ(transport->output.find("38;2;"), std::string::npos);
}

TEST(TerminalFrontend,
  LateRepliesPreserveCommandBufferAndNoOpCommandsDismissPrompt) {
  auto channel = EventChannel::Create();
  ASSERT_TRUE(channel);
  auto console = std::make_unique<ScriptConsole>();
  auto* transport = console.get();
  console->input = {Text(":"),
    Text("man 3 printf"),
    Key(ConsoleKey::kEnter),
    Text(":"),
    Text("back"),
    Key(ConsoleKey::kEnter),
    Text(":"),
    Text("search current"),
    Key(ConsoleKey::kEnter)};
  auto terminal = MakeTerminal(**channel, std::move(console));
  ASSERT_TRUE(terminal);
  Snapshot state;
  state.query = "current";
  ASSERT_TRUE((*terminal)->Render(state));
  EXPECT_TRUE(std::holds_alternative<Refresh>(Next(**terminal)));
  EXPECT_TRUE(std::holds_alternative<Refresh>(Next(**terminal)));
  ASSERT_TRUE((*channel)->TrySend(SearchCompleted{7, {}}));
  EXPECT_TRUE(std::holds_alternative<SearchCompleted>(Next(**terminal)));
  ASSERT_TRUE((*terminal)->Render(state));
  EXPECT_EQ(std::get<OpenPage>(Next(**terminal)).reference, "printf(3)");
  for (int n = 0; n < 6; ++n)
    EXPECT_TRUE(std::holds_alternative<Refresh>(Next(**terminal)));
  ASSERT_TRUE((*terminal)->Render(state));
  ASSERT_TRUE((*terminal)->Flush());
  EXPECT_EQ(transport->output.find("\x1b[?25h"), std::string::npos);
  EXPECT_EQ(state.query, "current");
}

TEST(TerminalFrontend, DirectManualStartupNeedsOneReadAndNoSearch) {
  auto channel = EventChannel::Create();
  ASSERT_TRUE(channel);
  auto console = std::make_unique<ScriptConsole>();
  auto* transport = console.get();
  console->input = {Text("q")};
  auto repository = WithManboManual();
  auto jobs = MakeJobService(*repository, **channel, 0);
  ASSERT_TRUE(jobs);
  auto terminal = MakeTerminal(**channel, std::move(console));
  ASSERT_TRUE(terminal);
  Session session(**terminal, **terminal, **jobs);
  auto result = session.RunInitial(OpenPage{"manbo(1)"});
  ASSERT_TRUE(result);
  EXPECT_EQ(result->requests, 1u);
  ASSERT_TRUE(session.browser().state().document);
  EXPECT_EQ(session.browser().state().document->id, "builtin/manbo/1");
  EXPECT_NE(transport->output.find("SYNOPSIS"), std::string::npos);
}
TEST(TerminalFrontend, TabSelectsReferencesAndEnterFollowsThem) {
  auto channel = EventChannel::Create();
  ASSERT_TRUE(channel);
  auto console = std::make_unique<ScriptConsole>();
  console->input = {Key(ConsoleKey::kTab),
    Key(ConsoleKey::kTab),
    Key(ConsoleKey::kBackTab),
    Key(ConsoleKey::kEnter)};
  auto terminal = MakeTerminal(**channel, std::move(console));
  ASSERT_TRUE(terminal);
  auto state = Reader();
  state.document = test::HtmlDocument(
    "<main><p><a href='first.html'>First</a> and "
    "<a href='second.html'>Second</a></p></main>");
  for (int index = 0; index < 3; ++index) {
    ASSERT_TRUE((*terminal)->Render(state));
    EXPECT_TRUE(std::holds_alternative<Refresh>(Next(**terminal)));
  }
  ASSERT_TRUE((*terminal)->Render(state));
  const auto follow = Next(**terminal);
  ASSERT_TRUE(std::holds_alternative<FollowLink>(follow));
  EXPECT_EQ(std::get<FollowLink>(follow).target, "first.html");
}

TEST(TerminalFrontend, ScrollingStartsAtTheResolvedAnchorAfterResize) {
  auto channel = EventChannel::Create();
  ASSERT_TRUE(channel);
  auto console = std::make_unique<ScriptConsole>();
  console->input = {Key(ConsoleKey::kDown)};
  auto terminal = MakeTerminal(**channel, std::move(console));
  ASSERT_TRUE(terminal);
  auto state = Reader();
  auto content = std::make_shared<RichDocument>(*state.document->content);
  content->blocks.push_back({BlockKind::kHeading, {{"Target"}}});
  content->blocks.back().anchor = "target";
  content->blocks.push_back(
    {BlockKind::kPreformatted, {{std::string(80, '\n') + "End"}}});
  state.document->content = content;
  state.document_anchor = "target";
  ASSERT_TRUE((*terminal)->Render(state));
  const auto event = Next(**terminal);
  ASSERT_TRUE(std::holds_alternative<Scroll>(event));
  const auto& scroll = std::get<Scroll>(event);
  ASSERT_TRUE(scroll.from_offset);
  EXPECT_GT(*scroll.from_offset, 40u);
  EXPECT_EQ(scroll.lines, 1);
}

}  // namespace
}  // namespace manbo
