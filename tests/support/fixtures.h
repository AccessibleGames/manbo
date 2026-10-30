#ifndef MANBO_TESTS_SUPPORT_FIXTURES_H_
#define MANBO_TESTS_SUPPORT_FIXTURES_H_

#include <filesystem>
#include <string>
#include <string_view>

#include "ui/terminal/view.h"

namespace manbo::test {
SearchHit
ExampleHit();
Document
ExampleDocument();
Document
HtmlDocument(std::string source);
Document
SamplePage(std::string_view id = "cpp/forward-list");
std::string
FrameText(const StyledFrame& frame);
bool
HasRole(const StyledFrame& frame, StyleRole role);
Snapshot
ResultsState();
Snapshot
ReaderState(Document document);
std::string
Bytes(const std::filesystem::path& path);
std::string
ContentText(const RichDocument& document);
}  // namespace manbo::test

#endif  // MANBO_TESTS_SUPPORT_FIXTURES_H_
