#include "core/command.h"

#include <gtest/gtest.h>

namespace manbo {
namespace {
TEST(Commands, DocumentationGrammarPreservesSectionAndQueryIntent) {
  auto list = ParseCommand(" :man printf ");
  ASSERT_TRUE(list);
  EXPECT_EQ(std::get<Search>(*list).query, "printf");
  auto read = ParseCommand("man 3p socket");
  ASSERT_TRUE(read);
  EXPECT_EQ(std::get<OpenPage>(*read).reference, "socket(3p)");
  EXPECT_EQ(
    std::get<OpenPage>(*ParseCommand("man printf(3)")).reference, "printf(3)");
  EXPECT_EQ(std::get<OpenPage>(*ParseCommand("open linux/printf/3")).reference,
    "linux/printf/3");
  EXPECT_EQ(
    std::get<Search>(*ParseCommand("manbo iterator invalidation")).query,
    "iterator invalidation");
  for (const auto* command : {"help", "manbo", "manbo manbo"})
    EXPECT_EQ(std::get<OpenPage>(*ParseCommand(command)).reference, "manbo(1)");
  EXPECT_TRUE(std::holds_alternative<Back>(*ParseCommand("back")));
  EXPECT_TRUE(std::holds_alternative<Quit>(*ParseCommand("q")));
  EXPECT_EQ(
    std::get<OpenPage>(*ParseStartupQuery("manbo")).reference, "manbo(1)");
  EXPECT_EQ(
    std::get<OpenPage>(*ParseStartupQuery("3 printf")).reference, "printf(3)");
  EXPECT_EQ(
    std::get<Search>(*ParseStartupQuery("std::vector")).query, "std::vector");
  for (const auto* invalid : {"man",
         "man 3",
         "man x printf",
         "man 3 printf extra",
         "open",
         "quit extra",
         "rm -rf anything",
         "help; echo wrong"})
    EXPECT_FALSE(ParseCommand(invalid)) << invalid;
  EXPECT_FALSE(ParseCommand(std::string(513, 'x')));
}

}  // namespace
}  // namespace manbo
