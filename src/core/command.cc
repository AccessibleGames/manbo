#include "core/command.h"

#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <ranges>

namespace manbo {
namespace {
auto
Trim(std::string_view text) -> std::string_view {
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == text.npos)
    return {};
  return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

auto
Words(std::string_view text) -> std::vector<std::string_view> {
  std::vector<std::string_view> words;
  while (!(text = Trim(text)).empty()) {
    const auto space = text.find_first_of(" \t\r\n");
    words.push_back(text.substr(0, space));
    if (space == text.npos)
      break;
    text.remove_prefix(space + 1);
  }
  return words;
}

auto
Section(std::string_view word) -> bool {
  if (word == "n" || word == "l")
    return true;
  return !word.empty() && word.size() <= 8 && word.front() >= '0' &&
         word.front() <= '9' && std::ranges::all_of(word, [](unsigned char c) {
           return std::isalnum(c) != 0;
         });
}

auto
Invalid(std::string text) -> Result<Event> {
  return tl::make_unexpected(
    Error{ErrorCode::kInvalidArgument, std::move(text)});
}
}  // namespace

auto
ParseCommand(std::string_view text) -> Result<Event> {
  text = Trim(text);
  if (text.starts_with(':'))
    text = Trim(text.substr(1));
  if (text.size() > 512)
    return Invalid("Command exceeds 512 bytes");
  const auto words = Words(text);
  if (words.empty())
    return Event{Refresh{}};
  const auto name = words.front();
  const auto argument = Trim(text.substr(name.size()));
  if (name == "q" || name == "quit")
    return argument.empty() ? Result<Event>{Quit{}} : Invalid("Usage: quit");
  if (name == "back")
    return argument.empty() ? Result<Event>{Back{}} : Invalid("Usage: back");
  if (name == "help" || name == "h" || name == "?")
    return argument.empty() ? Result<Event>{OpenPage{"manbo(1)"}}
                            : Invalid("Usage: help");
  if (name == "search")
    return Event{Search{std::string(argument)}};
  if (name == "open")
    return words.size() == 2 ? Result<Event>{OpenPage{std::string(words[1])}}
                             : Invalid("Usage: open name(section) or page-id");
  if (name == "manbo") {
    if (argument.empty() || argument == "manbo")
      return Event{OpenPage{"manbo(1)"}};
    return Event{Search{std::string(argument)}};
  }
  if (name == "man") {
    if (words.size() == 2) {
      if (Section(words[1]))
        return Invalid("Usage: man [section] name");
      if (words[1].ends_with(')') && words[1].find('(') != text.npos)
        return Event{OpenPage{std::string(words[1])}};
      return Event{Search{std::string(words[1])}};
    }
    if (words.size() == 3 && Section(words[1]))
      return Event{OpenPage{fmt::format("{}({})", words[2], words[1])}};
    return Invalid("Usage: man [section] name");
  }
  return Invalid(
    fmt::format("Unknown command: {}. Use :help for manbo(1).", name));
}

auto
ParseStartupQuery(std::string_view text) -> Result<Event> {
  text = Trim(text);
  if (text == "manbo")
    return Event{OpenPage{"manbo(1)"}};
  const auto words = Words(text);
  if (!words.empty() && (words.front() == "man" || words.front() == "manbo"))
    return ParseCommand(text);
  if (words.size() == 2 && Section(words.front()))
    return Event{OpenPage{fmt::format("{}({})", words[1], words[0])}};
  return Event{Search{std::string(text)}};
}
}  // namespace manbo
