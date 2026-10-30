// A real single-header consumer: no terminal UI, CLI framework, or GTest.
#include <iostream>

#include "manbo.h"

#if defined(MANBO_TERMINAL_TERMINAL_H_) || defined(MANBO_TERMINAL_VIEW_H_)
#error "The application facade must not pull in terminal UI headers"
#endif

int
main(int argc, char** argv) {
  // Exercise the public repository API against prepared indexes.
  // This path also runs without the terminal frontend or application target.
  if (argc == 5 && std::string_view(argv[1]) == "--follow") {
    auto offline = manbo::OfflineRepository::Open(argv[2]);
    if (!offline) {
      std::cerr << offline.error().message;
      return 1;
    }
    auto repository = manbo::WithManboManual(std::move(*offline));
    auto page = repository->Follow(argv[3], argv[4], {});
    if (!page) {
      std::cerr << page.error().message;
      return 1;
    }
    std::cout << page->document.id << '\n'
              << page->anchor << '\n'
              << page->document.body;
    return 0;
  }
  manbo::Browser browser;
  const auto change = browser.Dispatch(manbo::Search{"library consumer"});
  if (!change.visible_change || !change.request)
    return 1;
  auto channel = manbo::EventChannel::Create(2);
  if (!channel)
    return 2;
  if (!(*channel)->TrySend(manbo::Quit{}))
    return 3;
  auto event = (*channel)->TryReceive();
  if (!event || !std::holds_alternative<manbo::Quit>(*event))
    return 4;
  browser.Dispatch(*event);
  (*channel)->Close();
  if (browser.state().running || !(*channel)->closed() ||
      (*channel)->TryReceive())
    return 5;
  auto command = manbo::ParseCommand(":manbo manbo");
  if (!command || !std::holds_alternative<manbo::OpenPage>(*command))
    return 6;
  auto repository = manbo::WithManboManual();
  auto manual = repository->Read("manbo(1)", {});
  if (!manual || manual->id != "builtin/manbo/1" || !manual->content)
    return 7;
  return 0;
}
